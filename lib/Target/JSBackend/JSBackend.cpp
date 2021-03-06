//===-- JSBackend.cpp - Library for converting LLVM code to JS       -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements compiling of LLVM IR, which is assumed to have been
// simplified using the PNaCl passes, i64 legalization, and other necessary
// transformations, into JavaScript in asm.js format, suitable for passing
// to emscripten for final processing.
//
//===----------------------------------------------------------------------===//

#include "JSTargetMachine.h"
#include "MCTargetDesc/JSBackendMCTargetDesc.h"
#include "AllocaManager.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Config/config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/IR/DebugInfo.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set> // TODO: unordered_set?
using namespace llvm;

#include <OptPasses.h>
#include <Relooper.h>

#ifdef NDEBUG
#undef assert
#define assert(x) { if (!(x)) report_fatal_error(#x); }
#endif

raw_ostream &prettyWarning() {
  errs().changeColor(raw_ostream::YELLOW);
  errs() << "warning:";
  errs().resetColor();
  errs() << " ";
  return errs();
}

static cl::opt<bool>
PreciseF32("emscripten-precise-f32",
           cl::desc("Enables Math.fround usage to implement precise float32 semantics and performance (see emscripten PRECISE_F32 option)"),
           cl::init(false));

static cl::opt<bool>
WarnOnUnaligned("emscripten-warn-unaligned",
                cl::desc("Warns about unaligned loads and stores (which can negatively affect performance)"),
                cl::init(false));

static cl::opt<int>
ReservedFunctionPointers("emscripten-reserved-function-pointers",
                         cl::desc("Number of reserved slots in function tables for functions to be added at runtime (see emscripten RESERVED_FUNCTION_POINTERS option)"),
                         cl::init(0));

static cl::opt<int>
EmscriptenAssertions("emscripten-assertions",
                     cl::desc("Additional JS-specific assertions (see emscripten ASSERTIONS)"),
                     cl::init(0));

static cl::opt<bool>
NoAliasingFunctionPointers("emscripten-no-aliasing-function-pointers",
                           cl::desc("Forces function pointers to not alias (this is more correct, but rarely needed, and has the cost of much larger function tables; it is useful for debugging though; see emscripten ALIASING_FUNCTION_POINTERS option)"),
                           cl::init(false));

static cl::opt<int>
GlobalBase("emscripten-global-base",
           cl::desc("Where global variables start out in memory (see emscripten GLOBAL_BASE option)"),
           cl::init(8));


extern "C" void LLVMInitializeJSBackendTarget() {
  // Register the target.
  RegisterTargetMachine<JSTargetMachine> X(TheJSBackendTarget);
}

namespace {
  #define ASM_SIGNED 0
  #define ASM_UNSIGNED 1
  #define ASM_NONSPECIFIC 2 // nonspecific means to not differentiate ints. |0 for all, regardless of size and sign
  #define ASM_FFI_IN 4 // FFI return values are limited to things that work in ffis
  #define ASM_FFI_OUT 8 // params to FFIs are limited to things that work in ffis
  #define ASM_MUST_CAST 16 // this value must be explicitly cast (or be an integer constant)
  typedef unsigned AsmCast;

  const char *const SIMDLane = "XYZW";
  const char *const simdLane = "xyzw";

  typedef std::map<const Value*,std::string> ValueMap;
  typedef std::set<std::string> NameSet;
  typedef std::vector<unsigned char> HeapData;
  typedef std::pair<unsigned, unsigned> Address;
  typedef std::map<std::string, Type *> VarMap;
  typedef std::map<std::string, Address> GlobalAddressMap;
  typedef std::vector<std::string> FunctionTable;
  typedef std::map<std::string, FunctionTable> FunctionTableMap;
  typedef std::map<std::string, std::string> StringMap;
  typedef std::map<std::string, unsigned> NameIntMap;
  typedef std::map<const BasicBlock*, unsigned> BlockIndexMap;
  typedef std::map<const Function*, BlockIndexMap> BlockAddressMap;
  typedef std::map<const BasicBlock*, Block*> LLVMToRelooperMap;

  /// JSWriter - This class is the main chunk of code that converts an LLVM
  /// module to JavaScript.
  class JSWriter : public ModulePass {
    formatted_raw_ostream &Out;
    const Module *TheModule;
    unsigned UniqueNum;
    unsigned NextFunctionIndex; // used with NoAliasingFunctionPointers
    ValueMap ValueNames;
    VarMap UsedVars;
    AllocaManager Allocas;
    HeapData GlobalData8;
    HeapData GlobalData32;
    HeapData GlobalData64;
    GlobalAddressMap GlobalAddresses;
    NameSet Externals; // vars
    NameSet Declares; // funcs
    StringMap Redirects; // library function redirects actually used, needed for wrapper funcs in tables
    std::string PostSets;
    NameIntMap NamedGlobals; // globals that we export as metadata to JS, so it can access them by name
    std::map<std::string, unsigned> IndexedFunctions; // name -> index
    FunctionTableMap FunctionTables; // sig => list of functions
    std::vector<std::string> GlobalInitializers;
    std::vector<std::string> Exports; // additional exports
    BlockAddressMap BlockAddresses;

    std::string CantValidate;
    bool UsesSIMD;
    int InvokeState; // cycles between 0, 1 after preInvoke, 2 after call, 0 again after postInvoke. hackish, no argument there.
    CodeGenOpt::Level OptLevel;
    const DataLayout *DL;
    bool StackBumped;

    #include "CallHandlers.h"

  public:
    static char ID;
    JSWriter(formatted_raw_ostream &o, CodeGenOpt::Level OptLevel)
      : ModulePass(ID), Out(o), UniqueNum(0), NextFunctionIndex(0), CantValidate(""), UsesSIMD(false), InvokeState(0),
        OptLevel(OptLevel), StackBumped(false) {}

    virtual const char *getPassName() const { return "JavaScript backend"; }

    virtual bool runOnModule(Module &M);

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesAll();
      AU.addRequired<DataLayoutPass>();
      ModulePass::getAnalysisUsage(AU);
    }

    void printProgram(const std::string& fname, const std::string& modName );
    void printModule(const std::string& fname, const std::string& modName );
    void printFunction(const Function *F);

    LLVM_ATTRIBUTE_NORETURN void error(const std::string& msg);

    formatted_raw_ostream& nl(formatted_raw_ostream &Out, int delta = 0);

  private:
    void printCommaSeparated(const HeapData v);

    // parsing of constants has two phases: calculate, and then emit
    void parseConstant(const std::string& name, const Constant* CV, bool calculate);

    #define MEM_ALIGN 8
    #define MEM_ALIGN_BITS 64
    #define STACK_ALIGN 16
    #define STACK_ALIGN_BITS 128

    unsigned stackAlign(unsigned x) {
      return RoundUpToAlignment(x, STACK_ALIGN);
    }
    std::string stackAlignStr(std::string x) {
      return "((" + x + "+" + utostr(STACK_ALIGN-1) + ")&-" + utostr(STACK_ALIGN) + ")";
    }

    HeapData *allocateAddress(const std::string& Name, unsigned Bits = MEM_ALIGN_BITS) {
      assert(Bits == 64); // FIXME when we use optimal alignments
      HeapData *GlobalData = NULL;
      switch (Bits) {
        case 8:  GlobalData = &GlobalData8;  break;
        case 32: GlobalData = &GlobalData32; break;
        case 64: GlobalData = &GlobalData64; break;
        default: llvm_unreachable("Unsupported data element size");
      }
      while (GlobalData->size() % (Bits/8) != 0) GlobalData->push_back(0);
      GlobalAddresses[Name] = Address(GlobalData->size(), Bits);
      return GlobalData;
    }

    // return the absolute offset of a global
    unsigned getGlobalAddress(const std::string &s) {
      GlobalAddressMap::const_iterator I = GlobalAddresses.find(s);
      if (I == GlobalAddresses.end()) {
        report_fatal_error("cannot find global address " + Twine(s));
      }
      Address a = I->second;
      assert(a.second == 64); // FIXME when we use optimal alignments
      unsigned Ret;
      switch (a.second) {
        case 64:
          assert((a.first + GlobalBase)%8 == 0);
          Ret = a.first + GlobalBase;
          break;
        case 32:
          assert((a.first + GlobalBase)%4 == 0);
          Ret = a.first + GlobalBase + GlobalData64.size();
          break;
        case 8:
          Ret = a.first + GlobalBase + GlobalData64.size() + GlobalData32.size();
          break;
        default:
          report_fatal_error("bad global address " + Twine(s) + ": "
                             "count=" + Twine(a.first) + " "
                             "elementsize=" + Twine(a.second));
      }
      return Ret;
    }
    // returns the internal offset inside the proper block: GlobalData8, 32, 64
    unsigned getRelativeGlobalAddress(const std::string &s) {
      GlobalAddressMap::const_iterator I = GlobalAddresses.find(s);
      if (I == GlobalAddresses.end()) {
        report_fatal_error("cannot find global address " + Twine(s));
      }
      Address a = I->second;
      return a.first;
    }
    char getFunctionSignatureLetter(Type *T) {
      if (T->isVoidTy()) return 'v';
      else if (T->isFloatingPointTy()) {
        if (PreciseF32 && T->isFloatTy()) {
          return 'f';
        } else {
          return 'd';
        }
      } else if (VectorType *VT = dyn_cast<VectorType>(T)) {
        checkVectorType(VT);
        if (VT->getElementType()->isIntegerTy()) {
          return 'I';
        } else {
          return 'F';
        }
      } else {
        return 'i';
      }
    }
    std::string getFunctionSignature(const FunctionType *F, const std::string *Name=NULL) {
      std::string Ret;
      Ret += getFunctionSignatureLetter(F->getReturnType());
      for (FunctionType::param_iterator AI = F->param_begin(),
             AE = F->param_end(); AI != AE; ++AI) {
        Ret += getFunctionSignatureLetter(*AI);
      }
      return Ret;
    }
    FunctionTable& ensureFunctionTable(const FunctionType *FT) {
      FunctionTable &Table = FunctionTables[getFunctionSignature(FT)];
      unsigned MinSize = ReservedFunctionPointers ? 2*(ReservedFunctionPointers+1) : 1; // each reserved slot must be 2-aligned
      while (Table.size() < MinSize) Table.push_back("0");
      return Table;
    }
    unsigned getFunctionIndex(const Function *F) {
      const std::string &Name = getJSName(F);
      if (IndexedFunctions.find(Name) != IndexedFunctions.end()) return IndexedFunctions[Name];
      std::string Sig = getFunctionSignature(F->getFunctionType(), &Name);
      FunctionTable& Table = ensureFunctionTable(F->getFunctionType());
      if (NoAliasingFunctionPointers) {
        while (Table.size() < NextFunctionIndex) Table.push_back("0");
      }
      unsigned Alignment = F->getAlignment() || 1; // XXX this is wrong, it's always 1. but, that's fine in the ARM-like ABI we have which allows unaligned functions.
                                                   //     the one risk is if someone forces a function to be aligned, and relies on that.
      while (Table.size() % Alignment) Table.push_back("0");
      unsigned Index = Table.size();
      Table.push_back(Name);
      IndexedFunctions[Name] = Index;
      if (NoAliasingFunctionPointers) {
        NextFunctionIndex = Index+1;
      }

      // invoke the callHandler for this, if there is one. the function may only be indexed but never called directly, and we may need to do things in the handler
      CallHandlerMap::const_iterator CH = CallHandlers.find(Name);
      if (CH != CallHandlers.end()) {
        (this->*(CH->second))(NULL, Name, -1);
      }

      return Index;
    }

    unsigned getBlockAddress(const Function *F, const BasicBlock *BB) {
      BlockIndexMap& Blocks = BlockAddresses[F];
      if (Blocks.find(BB) == Blocks.end()) {
        Blocks[BB] = Blocks.size(); // block addresses start from 0
      }
      return Blocks[BB];
    }

    unsigned getBlockAddress(const BlockAddress *BA) {
      return getBlockAddress(BA->getFunction(), BA->getBasicBlock());
    }

    const Value *resolveFully(const Value *V) {
      bool More = true;
      while (More) {
        More = false;
        if (const GlobalAlias *GA = dyn_cast<GlobalAlias>(V)) {
          V = GA->getAliasee();
          More = true;
        }
        if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(V)) {
          V = CE->getOperand(0); // ignore bitcasts
          More = true;
        }
      }
      return V;
    }

    // Return a constant we are about to write into a global as a numeric offset. If the
    // value is not known at compile time, emit a postSet to that location.
    unsigned getConstAsOffset(const Value *V, unsigned AbsoluteTarget) {
      V = resolveFully(V);
      if (const Function *F = dyn_cast<const Function>(V)) {
        return getFunctionIndex(F);
      } else if (const BlockAddress *BA = dyn_cast<const BlockAddress>(V)) {
        return getBlockAddress(BA);
      } else {
        if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(V)) {
          if (!GV->hasInitializer()) {
            // We don't have a constant to emit here, so we must emit a postSet
            // All postsets are of external values, so they are pointers, hence 32-bit
            std::string Name = getOpName(V);
            Externals.insert(Name);
            PostSets += "HEAP32[" + utostr(AbsoluteTarget>>2) + "] = " + Name + ';';
            return 0; // emit zero in there for now, until the postSet
          }
        }
        return getGlobalAddress(V->getName().str());
      }
    }

    // Test whether the given value is known to be an absolute value or one we turn into an absolute value
    bool isAbsolute(const Value *P) {
      if (const IntToPtrInst *ITP = dyn_cast<IntToPtrInst>(P)) {
        return isa<ConstantInt>(ITP->getOperand(0));
      }
      if (isa<ConstantPointerNull>(P) || isa<UndefValue>(P)) {
        return true;
      }
      return false;
    }

    void checkVectorType(Type *T) {
      VectorType *VT = cast<VectorType>(T);
      // LLVM represents the results of vector comparison as vectors of i1. We
      // represent them as vectors of integers the size of the vector elements
      // of the compare that produced them.
      assert(VT->getElementType()->getPrimitiveSizeInBits() == 32 ||
             VT->getElementType()->getPrimitiveSizeInBits() == 1);
      assert(VT->getBitWidth() <= 128);
      assert(VT->getNumElements() <= 4);
      UsesSIMD = true;
    }

    std::string ensureCast(std::string S, Type *T, AsmCast sign) {
      if (sign & ASM_MUST_CAST) return getCast(S, T);
      return S;
    }

    std::string ftostr(const ConstantFP *CFP, AsmCast sign) {
      const APFloat &flt = CFP->getValueAPF();

      // Emscripten has its own spellings for infinity and NaN.
      if (flt.getCategory() == APFloat::fcInfinity) return ensureCast(flt.isNegative() ? "-inf" : "inf", CFP->getType(), sign);
      else if (flt.getCategory() == APFloat::fcNaN) return ensureCast("nan", CFP->getType(), sign);

      // Request 9 or 17 digits, aka FLT_DECIMAL_DIG or DBL_DECIMAL_DIG (our
      // long double is the the same as our double), to avoid rounding errors.
      SmallString<29> Str;
      flt.toString(Str, PreciseF32 && CFP->getType()->isFloatTy() ? 9 : 17);

      // asm.js considers literals to be floating-point literals when they contain a
      // dot, however our output may be processed by UglifyJS, which doesn't
      // currently preserve dots in all cases. Mark floating-point literals with
      // unary plus to force them to floating-point.
      if (APFloat(flt).roundToIntegral(APFloat::rmNearestTiesToEven) == APFloat::opOK) {
        return '+' + Str.str().str();
      }

      return Str.str().str();
    }

    std::string getPtrLoad(const Value* Ptr);
    std::string getHeapAccess(const std::string& Name, unsigned Bytes, bool Integer=true);
    std::string getPtrUse(const Value* Ptr);
    std::string getConstant(const Constant*, AsmCast sign=ASM_SIGNED);
    std::string getConstantVector(Type *ElementType, std::string x, std::string y, std::string z, std::string w);
    std::string getValueAsStr(const Value*, AsmCast sign=ASM_SIGNED);
    std::string getValueAsCastStr(const Value*, AsmCast sign=ASM_SIGNED);
    std::string getValueAsParenStr(const Value*);
    std::string getValueAsCastParenStr(const Value*, AsmCast sign=ASM_SIGNED);

    const std::string &getJSName(const Value* val);

    std::string getPhiCode(const BasicBlock *From, const BasicBlock *To);

    void printAttributes(const AttributeSet &PAL, const std::string &name);
    void printType(Type* Ty);
    void printTypes(const Module* M);

    std::string getAdHocAssign(const StringRef &, Type *);
    std::string getAssign(const Instruction *I);
    std::string getAssignIfNeeded(const Value *V);
    std::string getCast(const StringRef &, Type *, AsmCast sign=ASM_SIGNED);
    std::string getParenCast(const StringRef &, Type *, AsmCast sign=ASM_SIGNED);
    std::string getDoubleToInt(const StringRef &);
    std::string getIMul(const Value *, const Value *);
    std::string getLoad(const Instruction *I, const Value *P, Type *T, unsigned Alignment, char sep=';');
    std::string getStore(const Instruction *I, const Value *P, Type *T, const std::string& VS, unsigned Alignment, char sep=';');
    std::string getStackBump(unsigned Size);
    std::string getStackBump(const std::string &Size);

    void addBlock(const BasicBlock *BB, Relooper& R, LLVMToRelooperMap& LLVMToRelooper);
    void printFunctionBody(const Function *F);
    void generateInsertElementExpression(const InsertElementInst *III, raw_string_ostream& Code);
    void generateExtractElementExpression(const ExtractElementInst *EEI, raw_string_ostream& Code);
    void generateShuffleVectorExpression(const ShuffleVectorInst *SVI, raw_string_ostream& Code);
    void generateICmpExpression(const ICmpInst *I, raw_string_ostream& Code);
    void generateFCmpExpression(const FCmpInst *I, raw_string_ostream& Code);
    void generateShiftExpression(const BinaryOperator *I, raw_string_ostream& Code);
    void generateUnrolledExpression(const User *I, raw_string_ostream& Code);
    bool generateSIMDExpression(const User *I, raw_string_ostream& Code);
    void generateExpression(const User *I, raw_string_ostream& Code);

    std::string getOpName(const Value*);

    void processConstants();

    // nativization

    typedef std::set<const Value*> NativizedVarsMap;
    NativizedVarsMap NativizedVars;

    void calculateNativizedVars(const Function *F);

    // special analyses

    bool canReloop(const Function *F);

    // main entry point

    void printModuleBody();
  };
} // end anonymous namespace.

formatted_raw_ostream &JSWriter::nl(formatted_raw_ostream &Out, int delta) {
  Out << '\n';
  return Out;
}

static inline char halfCharToHex(unsigned char half) {
  assert(half <= 15);
  if (half <= 9) {
    return '0' + half;
  } else {
    return 'A' + half - 10;
  }
}

static inline void sanitizeGlobal(std::string& str) {
  // Global names are prefixed with "_" to prevent them from colliding with
  // names of things in normal JS.
  str = "_" + str;

  // functions and globals should already be in C-style format,
  // in addition to . for llvm intrinsics and possibly $ and so forth.
  // There is a risk of collisions here, we just lower all these
  // invalid characters to _, but this should not happen in practice.
  // TODO: in debug mode, check for such collisions.
  size_t OriginalSize = str.size();
  for (size_t i = 1; i < OriginalSize; ++i) {
    unsigned char c = str[i];
    if (!isalnum(c) && c != '_') str[i] = '_';
  }
}

static inline void sanitizeLocal(std::string& str) {
  // Local names are prefixed with "$" to prevent them from colliding with
  // global names.
  str = "$" + str;

  // We need to convert every string that is not a valid JS identifier into
  // a valid one, without collisions - we cannot turn "x.a" into "x_a" while
  // also leaving "x_a" as is, for example.
  //
  // We leave valid characters 0-9a-zA-Z and _ unchanged. Anything else
  // we replace with $ and append a hex representation of that value,
  // so for example x.a turns into x$a2e, x..a turns into x$$a2e2e.
  //
  // As an optimization, we replace . with $ without appending anything,
  // unless there is another illegal character. The reason is that . is
  // a common illegal character, and we want to avoid resizing strings
  // for perf reasons, and we If we do see we need to append something, then
  // for . we just append Z (one character, instead of the hex code).
  //

  size_t OriginalSize = str.size();
  int Queued = 0;
  for (size_t i = 1; i < OriginalSize; ++i) {
    unsigned char c = str[i];
    if (!isalnum(c) && c != '_') {
      str[i] = '$';
      if (c == '.') {
        Queued++;
      } else {
        size_t s = str.size();
        str.resize(s+2+Queued);
        for (int i = 0; i < Queued; i++) {
          str[s++] = 'Z';
        }
        Queued = 0;
        str[s] = halfCharToHex(c >> 4);
        str[s+1] = halfCharToHex(c & 0xf);
      }
    }
  }
}

static inline std::string ensureFloat(const std::string &S, Type *T) {
  if (PreciseF32 && T->isFloatTy()) {
    return "Math_fround(" + S + ")";
  }
  return S;
}

static void emitDebugInfo(raw_ostream& Code, const Instruction *I) {
  if (MDNode *N = I->getMetadata("dbg")) {
    DILocation Loc(N);
    unsigned Line = Loc.getLineNumber();
    StringRef File = Loc.getFilename();
    Code << " //@line " << utostr(Line) << " \"" << (File.size() > 0 ? File.str() : "?") << "\"";
  }
}

void JSWriter::error(const std::string& msg) {
  report_fatal_error(msg);
}

std::string JSWriter::getPhiCode(const BasicBlock *From, const BasicBlock *To) {
  // FIXME this is all quite inefficient, and also done once per incoming to each phi

  // Find the phis, and generate assignments and dependencies
  std::set<std::string> PhiVars;
  for (BasicBlock::const_iterator I = To->begin(), E = To->end();
       I != E; ++I) {
    const PHINode* P = dyn_cast<PHINode>(I);
    if (!P) break;
    PhiVars.insert(getJSName(P));
  }
  typedef std::map<std::string, std::string> StringMap;
  StringMap assigns; // variable -> assign statement
  std::map<std::string, const Value*> values; // variable -> Value
  StringMap deps; // variable -> dependency
  StringMap undeps; // reverse: dependency -> variable
  for (BasicBlock::const_iterator I = To->begin(), E = To->end();
       I != E; ++I) {
    const PHINode* P = dyn_cast<PHINode>(I);
    if (!P) break;
    int index = P->getBasicBlockIndex(From);
    if (index < 0) continue;
    // we found it
    const std::string &name = getJSName(P);
    assigns[name] = getAssign(P);
    // Get the operand, and strip pointer casts, since normal expression
    // translation also strips pointer casts, and we want to see the same
    // thing so that we can detect any resulting dependencies.
    const Value *V = P->getIncomingValue(index)->stripPointerCasts();
    values[name] = V;
    std::string vname = getValueAsStr(V);
    if (const Instruction *VI = dyn_cast<const Instruction>(V)) {
      if (VI->getParent() == To && PhiVars.find(vname) != PhiVars.end()) {
        deps[name] = vname;
        undeps[vname] = name;
      }
    }
  }
  // Emit assignments+values, taking into account dependencies, and breaking cycles
  std::string pre = "", post = "";
  while (assigns.size() > 0) {
    bool emitted = false;
    for (StringMap::iterator I = assigns.begin(); I != assigns.end();) {
      StringMap::iterator last = I;
      std::string curr = last->first;
      const Value *V = values[curr];
      std::string CV = getValueAsStr(V);
      I++; // advance now, as we may erase
      // if we have no dependencies, or we found none to emit and are at the end (so there is a cycle), emit
      StringMap::const_iterator dep = deps.find(curr);
      if (dep == deps.end() || (!emitted && I == assigns.end())) {
        if (dep != deps.end()) {
          // break a cycle
          std::string depString = dep->second;
          std::string temp = curr + "$phi";
          pre += getAdHocAssign(temp, V->getType()) + CV + ';';
          CV = temp;
          deps.erase(curr);
          undeps.erase(depString);
        }
        post += assigns[curr] + CV + ';';
        assigns.erase(last);
        emitted = true;
      }
    }
  }
  return pre + post;
}

const std::string &JSWriter::getJSName(const Value* val) {
  ValueMap::const_iterator I = ValueNames.find(val);
  if (I != ValueNames.end() && I->first == val)
    return I->second;

  // If this is an alloca we've replaced with another, use the other name.
  if (const AllocaInst *AI = dyn_cast<AllocaInst>(val)) {
    if (AI->isStaticAlloca()) {
      const AllocaInst *Rep = Allocas.getRepresentative(AI);
      if (Rep != AI) {
        return getJSName(Rep);
      }
    }
  }

  std::string name;
  if (val->hasName()) {
    name = val->getName().str();
  } else {
    name = utostr(UniqueNum++);
  }

  if (isa<Constant>(val)) {
    sanitizeGlobal(name);
  } else {
    sanitizeLocal(name);
  }

  return ValueNames[val] = name;
}

std::string JSWriter::getAdHocAssign(const StringRef &s, Type *t) {
  UsedVars[s] = t;
  return (s + " = ").str();
}

std::string JSWriter::getAssign(const Instruction *I) {
  return getAdHocAssign(getJSName(I), I->getType());
}

std::string JSWriter::getAssignIfNeeded(const Value *V) {
  if (const Instruction *I = dyn_cast<Instruction>(V)) {
    if (!I->use_empty()) return getAssign(I);
  }
  return std::string();
}

std::string JSWriter::getCast(const StringRef &s, Type *t, AsmCast sign) {
  switch (t->getTypeID()) {
    default: {
      errs() << *t << "\n";
      assert(false && "Unsupported type");
    }
    case Type::VectorTyID:
      return (cast<VectorType>(t)->getElementType()->isIntegerTy() ?
              "SIMD_int32x4_check(" + s + ")" :
              "SIMD_float32x4_check(" + s + ")").str();
    case Type::FloatTyID: {
      if (PreciseF32 && !(sign & ASM_FFI_OUT)) {
        if (sign & ASM_FFI_IN) {
          return ("Math_fround(+(" + s + "))").str();
        } else {
          return ("Math_fround(" + s + ")").str();
        }
      }
      // otherwise fall through to double
    }
    case Type::DoubleTyID: return ("+" + s).str();
    case Type::IntegerTyID: {
      // fall through to the end for nonspecific
      switch (t->getIntegerBitWidth()) {
        case 1:  if (!(sign & ASM_NONSPECIFIC)) return sign == ASM_UNSIGNED ? (s + "&1").str()     : (s + "<<31>>31").str();
        case 8:  if (!(sign & ASM_NONSPECIFIC)) return sign == ASM_UNSIGNED ? (s + "&255").str()   : (s + "<<24>>24").str();
        case 16: if (!(sign & ASM_NONSPECIFIC)) return sign == ASM_UNSIGNED ? (s + "&65535").str() : (s + "<<16>>16").str();
        case 32: return (sign == ASM_SIGNED || (sign & ASM_NONSPECIFIC) ? s + "|0" : s + ">>>0").str();
        default: llvm_unreachable("Unsupported integer cast bitwidth");
      }
    }
    case Type::PointerTyID:
      return (sign == ASM_SIGNED || (sign & ASM_NONSPECIFIC) ? s + "|0" : s + ">>>0").str();
  }
}

std::string JSWriter::getParenCast(const StringRef &s, Type *t, AsmCast sign) {
  return getCast(("(" + s + ")").str(), t, sign);
}

std::string JSWriter::getDoubleToInt(const StringRef &s) {
  return ("~~(" + s + ")").str();
}

std::string JSWriter::getIMul(const Value *V1, const Value *V2) {
  const ConstantInt *CI = NULL;
  const Value *Other = NULL;
  if ((CI = dyn_cast<ConstantInt>(V1))) {
    Other = V2;
  } else if ((CI = dyn_cast<ConstantInt>(V2))) {
    Other = V1;
  }
  // we ignore optimizing the case of multiplying two constants - optimizer would have removed those
  if (CI) {
    std::string OtherStr = getValueAsStr(Other);
    unsigned C = CI->getZExtValue();
    if (C == 0) return "0";
    if (C == 1) return OtherStr;
    unsigned Orig = C, Shifts = 0;
    while (C) {
      if ((C & 1) && (C != 1)) break; // not power of 2
      C >>= 1;
      Shifts++;
      if (C == 0) return OtherStr + "<<" + utostr(Shifts-1); // power of 2, emit shift
    }
    if (Orig < (1<<20)) return "(" + OtherStr + "*" + utostr(Orig) + ")|0"; // small enough, avoid imul
  }
  return "Math_imul(" + getValueAsStr(V1) + ", " + getValueAsStr(V2) + ")|0"; // unknown or too large, emit imul
}

std::string JSWriter::getLoad(const Instruction *I, const Value *P, Type *T, unsigned Alignment, char sep) {
  std::string Assign = getAssign(I);
  unsigned Bytes = DL->getTypeAllocSize(T);
  std::string text;
  if (Bytes <= Alignment || Alignment == 0) {
    text = Assign + getPtrLoad(P);
    if (isAbsolute(P)) {
      // loads from an absolute constants are either intentional segfaults (int x = *((int*)0)), or code problems
      text += "; abort() /* segfault, load from absolute addr */";
    }
  } else {
    // unaligned in some manner
    if (WarnOnUnaligned) {
      errs() << "emcc: warning: unaligned load in  " << I->getParent()->getParent()->getName() << ":" << *I << " | ";
      emitDebugInfo(errs(), I);
      errs() << "\n";
    }
    std::string PS = getValueAsStr(P);
    switch (Bytes) {
      case 8: {
        switch (Alignment) {
          case 4: {
            text = "HEAP32[tempDoublePtr>>2]=HEAP32[" + PS + ">>2]" + sep +
                    "HEAP32[tempDoublePtr+4>>2]=HEAP32[" + PS + "+4>>2]";
            break;
          }
          case 2: {
            text = "HEAP16[tempDoublePtr>>1]=HEAP16[" + PS + ">>1]" + sep +
                   "HEAP16[tempDoublePtr+2>>1]=HEAP16[" + PS + "+2>>1]" + sep +
                   "HEAP16[tempDoublePtr+4>>1]=HEAP16[" + PS + "+4>>1]" + sep +
                   "HEAP16[tempDoublePtr+6>>1]=HEAP16[" + PS + "+6>>1]";
            break;
          }
          case 1: {
            text = "HEAP8[tempDoublePtr>>0]=HEAP8[" + PS + ">>0]" + sep +
                   "HEAP8[tempDoublePtr+1>>0]=HEAP8[" + PS + "+1>>0]" + sep +
                   "HEAP8[tempDoublePtr+2>>0]=HEAP8[" + PS + "+2>>0]" + sep +
                   "HEAP8[tempDoublePtr+3>>0]=HEAP8[" + PS + "+3>>0]" + sep +
                   "HEAP8[tempDoublePtr+4>>0]=HEAP8[" + PS + "+4>>0]" + sep +
                   "HEAP8[tempDoublePtr+5>>0]=HEAP8[" + PS + "+5>>0]" + sep +
                   "HEAP8[tempDoublePtr+6>>0]=HEAP8[" + PS + "+6>>0]" + sep +
                   "HEAP8[tempDoublePtr+7>>0]=HEAP8[" + PS + "+7>>0]";
            break;
          }
          default: assert(0 && "bad 8 store");
        }
        text += sep + Assign + "+HEAPF64[tempDoublePtr>>3]";
        break;
      }
      case 4: {
        if (T->isIntegerTy() || T->isPointerTy()) {
          switch (Alignment) {
            case 2: {
              text = Assign + "HEAPU16[" + PS + ">>1]|" +
                             "(HEAPU16[" + PS + "+2>>1]<<16)";
              break;
            }
            case 1: {
              text = Assign + "HEAPU8[" + PS + ">>0]|" +
                             "(HEAPU8[" + PS + "+1>>0]<<8)|" +
                             "(HEAPU8[" + PS + "+2>>0]<<16)|" +
                             "(HEAPU8[" + PS + "+3>>0]<<24)";
              break;
            }
            default: assert(0 && "bad 4i store");
          }
        } else { // float
          assert(T->isFloatingPointTy());
          switch (Alignment) {
            case 2: {
              text = "HEAP16[tempDoublePtr>>1]=HEAP16[" + PS + ">>1]" + sep +
                     "HEAP16[tempDoublePtr+2>>1]=HEAP16[" + PS + "+2>>1]";
              break;
            }
            case 1: {
              text = "HEAP8[tempDoublePtr>>0]=HEAP8[" + PS + ">>0]" + sep +
                     "HEAP8[tempDoublePtr+1>>0]=HEAP8[" + PS + "+1>>0]" + sep +
                     "HEAP8[tempDoublePtr+2>>0]=HEAP8[" + PS + "+2>>0]" + sep +
                     "HEAP8[tempDoublePtr+3>>0]=HEAP8[" + PS + "+3>>0]";
              break;
            }
            default: assert(0 && "bad 4f store");
          }
          text += sep + Assign + getCast("HEAPF32[tempDoublePtr>>2]", Type::getFloatTy(TheModule->getContext()));
        }
        break;
      }
      case 2: {
        text = Assign + "HEAPU8[" + PS + ">>0]|" +
                       "(HEAPU8[" + PS + "+1>>0]<<8)";
        break;
      }
      default: assert(0 && "bad store");
    }
  }
  return text;
}

std::string JSWriter::getStore(const Instruction *I, const Value *P, Type *T, const std::string& VS, unsigned Alignment, char sep) {
  assert(sep == ';'); // FIXME when we need that
  unsigned Bytes = DL->getTypeAllocSize(T);
  std::string text;
  if (Bytes <= Alignment || Alignment == 0) {
    text = getPtrUse(P) + " = " + VS;
    if (Alignment == 536870912) text += "; abort() /* segfault */";
  } else {
    // unaligned in some manner
    if (WarnOnUnaligned) {
      errs() << "emcc: warning: unaligned store in " << I->getParent()->getParent()->getName() << ":" << *I << " | ";
      emitDebugInfo(errs(), I);
      errs() << "\n";
    }
    std::string PS = getValueAsStr(P);
    switch (Bytes) {
      case 8: {
        text = "HEAPF64[tempDoublePtr>>3]=" + VS + ';';
        switch (Alignment) {
          case 4: {
            text += "HEAP32[" + PS + ">>2]=HEAP32[tempDoublePtr>>2];" +
                    "HEAP32[" + PS + "+4>>2]=HEAP32[tempDoublePtr+4>>2]";
            break;
          }
          case 2: {
            text += "HEAP16[" + PS + ">>1]=HEAP16[tempDoublePtr>>1];" +
                    "HEAP16[" + PS + "+2>>1]=HEAP16[tempDoublePtr+2>>1];" +
                    "HEAP16[" + PS + "+4>>1]=HEAP16[tempDoublePtr+4>>1];" +
                    "HEAP16[" + PS + "+6>>1]=HEAP16[tempDoublePtr+6>>1]";
            break;
          }
          case 1: {
            text += "HEAP8[" + PS + ">>0]=HEAP8[tempDoublePtr>>0];" +
                    "HEAP8[" + PS + "+1>>0]=HEAP8[tempDoublePtr+1>>0];" +
                    "HEAP8[" + PS + "+2>>0]=HEAP8[tempDoublePtr+2>>0];" +
                    "HEAP8[" + PS + "+3>>0]=HEAP8[tempDoublePtr+3>>0];" +
                    "HEAP8[" + PS + "+4>>0]=HEAP8[tempDoublePtr+4>>0];" +
                    "HEAP8[" + PS + "+5>>0]=HEAP8[tempDoublePtr+5>>0];" +
                    "HEAP8[" + PS + "+6>>0]=HEAP8[tempDoublePtr+6>>0];" +
                    "HEAP8[" + PS + "+7>>0]=HEAP8[tempDoublePtr+7>>0]";
            break;
          }
          default: assert(0 && "bad 8 store");
        }
        break;
      }
      case 4: {
        if (T->isIntegerTy() || T->isPointerTy()) {
          switch (Alignment) {
            case 2: {
              text = "HEAP16[" + PS + ">>1]=" + VS + "&65535;" +
                     "HEAP16[" + PS + "+2>>1]=" + VS + ">>>16";
              break;
            }
            case 1: {
              text = "HEAP8[" + PS + ">>0]=" + VS + "&255;" +
                     "HEAP8[" + PS + "+1>>0]=(" + VS + ">>8)&255;" +
                     "HEAP8[" + PS + "+2>>0]=(" + VS + ">>16)&255;" +
                     "HEAP8[" + PS + "+3>>0]=" + VS + ">>24";
              break;
            }
            default: assert(0 && "bad 4i store");
          }
        } else { // float
          assert(T->isFloatingPointTy());
          text = "HEAPF32[tempDoublePtr>>2]=" + VS + ';';
          switch (Alignment) {
            case 2: {
              text += "HEAP16[" + PS + ">>1]=HEAP16[tempDoublePtr>>1];" +
                      "HEAP16[" + PS + "+2>>1]=HEAP16[tempDoublePtr+2>>1]";
              break;
            }
            case 1: {
              text += "HEAP8[" + PS + ">>0]=HEAP8[tempDoublePtr>>0];" +
                      "HEAP8[" + PS + "+1>>0]=HEAP8[tempDoublePtr+1>>0];" +
                      "HEAP8[" + PS + "+2>>0]=HEAP8[tempDoublePtr+2>>0];" +
                      "HEAP8[" + PS + "+3>>0]=HEAP8[tempDoublePtr+3>>0]";
              break;
            }
            default: assert(0 && "bad 4f store");
          }
        }
        break;
      }
      case 2: {
        text = "HEAP8[" + PS + ">>0]=" + VS + "&255;" +
               "HEAP8[" + PS + "+1>>0]=" + VS + ">>8";
        break;
      }
      default: assert(0 && "bad store");
    }
  }
  return text;
}

std::string JSWriter::getStackBump(unsigned Size) {
  return getStackBump(utostr(Size));
}

std::string JSWriter::getStackBump(const std::string &Size) {
  std::string ret = "STACKTOP = STACKTOP + " + Size + "|0;";
  if (EmscriptenAssertions) {
    ret += " if ((STACKTOP|0) >= (STACK_MAX|0)) abort();";
  }
  return ret;
}

std::string JSWriter::getOpName(const Value* V) { // TODO: remove this
  return getJSName(V);
}

std::string JSWriter::getPtrLoad(const Value* Ptr) {
  Type *t = cast<PointerType>(Ptr->getType())->getElementType();
  return getCast(getPtrUse(Ptr), t, ASM_NONSPECIFIC);
}

std::string JSWriter::getHeapAccess(const std::string& Name, unsigned Bytes, bool Integer) {
  switch (Bytes) {
  default: llvm_unreachable("Unsupported type");
  case 8: return "HEAPF64[" + Name + ">>3]";
  case 4: {
    if (Integer) {
      return "HEAP32[" + Name + ">>2]";
    } else {
      return "HEAPF32[" + Name + ">>2]";
    }
  }
  case 2: return "HEAP16[" + Name + ">>1]";
  case 1: return "HEAP8[" + Name + ">>0]";
  }
}

std::string JSWriter::getPtrUse(const Value* Ptr) {
  Type *t = cast<PointerType>(Ptr->getType())->getElementType();
  unsigned Bytes = DL->getTypeAllocSize(t);
  if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(Ptr)) {
    std::string text = "";
    unsigned Addr = getGlobalAddress(GV->getName().str());
    switch (Bytes) {
    default: llvm_unreachable("Unsupported type");
    case 8: return "HEAPF64[" + utostr(Addr >> 3) + "]";
    case 4: {
      if (t->isIntegerTy() || t->isPointerTy()) {
        return "HEAP32[" + utostr(Addr >> 2) + "]";
      } else {
        assert(t->isFloatingPointTy());
        return "HEAPF32[" + utostr(Addr >> 2) + "]";
      }
    }
    case 2: return "HEAP16[" + utostr(Addr >> 1) + "]";
    case 1: return "HEAP8[" + utostr(Addr) + "]";
    }
  } else {
    return getHeapAccess(getValueAsStr(Ptr), Bytes, t->isIntegerTy() || t->isPointerTy());
  }
}

std::string JSWriter::getConstant(const Constant* CV, AsmCast sign) {
  if (isa<ConstantPointerNull>(CV)) return "0";

  if (const Function *F = dyn_cast<Function>(CV)) {
    return utostr(getFunctionIndex(F));
  }

  if (const GlobalValue *GV = dyn_cast<GlobalValue>(CV)) {
    if (GV->isDeclaration()) {
      std::string Name = getOpName(GV);
      Externals.insert(Name);
      return Name;
    }
    if (const GlobalAlias *GA = dyn_cast<GlobalAlias>(CV)) {
      // Since we don't currently support linking of our output, we don't need
      // to worry about weak or other kinds of aliases.
      return getConstant(GA->getAliasee(), sign);
    }
    return utostr(getGlobalAddress(GV->getName().str()));
  }

  if (const ConstantFP *CFP = dyn_cast<ConstantFP>(CV)) {
    std::string S = ftostr(CFP, sign);
    if (PreciseF32 && CV->getType()->isFloatTy() && !(sign & ASM_FFI_OUT)) {
      S = "Math_fround(" + S + ")";
    }
    return S;
  } else if (const ConstantInt *CI = dyn_cast<ConstantInt>(CV)) {
    if (sign != ASM_UNSIGNED && CI->getValue().getBitWidth() == 1) {
      sign = ASM_UNSIGNED; // bools must always be unsigned: either 0 or 1
    }
    return CI->getValue().toString(10, sign != ASM_UNSIGNED);
  } else if (isa<UndefValue>(CV)) {
    std::string S;
    if (VectorType *VT = dyn_cast<VectorType>(CV->getType())) {
      checkVectorType(VT);
      if (VT->getElementType()->isIntegerTy()) {
        S = "SIMD_int32x4_splat(0)";
      } else {
        S = "SIMD_float32x4_splat(Math_fround(0))";
      }
    } else {
      S = CV->getType()->isFloatingPointTy() ? "+0" : "0"; // XXX refactor this
      if (PreciseF32 && CV->getType()->isFloatTy() && !(sign & ASM_FFI_OUT)) {
        S = "Math_fround(" + S + ")";
      }
    }
    return S;
  } else if (isa<ConstantAggregateZero>(CV)) {
    if (VectorType *VT = dyn_cast<VectorType>(CV->getType())) {
      checkVectorType(VT);
      if (VT->getElementType()->isIntegerTy()) {
        return "SIMD_int32x4_splat(0)";
      } else {
        return "SIMD_float32x4_splat(Math_fround(0))";
      }
    } else {
      // something like [0 x i8*] zeroinitializer, which clang can emit for landingpads
      return "0";
    }
  } else if (const ConstantDataVector *DV = dyn_cast<ConstantDataVector>(CV)) {
    checkVectorType(DV->getType());
    unsigned NumElts = cast<VectorType>(DV->getType())->getNumElements();
    Type *EltTy = cast<VectorType>(DV->getType())->getElementType();
    Constant *Undef = UndefValue::get(EltTy);
    return getConstantVector(EltTy,
                             getConstant(NumElts > 0 ? DV->getElementAsConstant(0) : Undef),
                             getConstant(NumElts > 1 ? DV->getElementAsConstant(1) : Undef),
                             getConstant(NumElts > 2 ? DV->getElementAsConstant(2) : Undef),
                             getConstant(NumElts > 3 ? DV->getElementAsConstant(3) : Undef));
  } else if (const ConstantVector *V = dyn_cast<ConstantVector>(CV)) {
    checkVectorType(V->getType());
    unsigned NumElts = cast<VectorType>(CV->getType())->getNumElements();
    Type *EltTy = cast<VectorType>(CV->getType())->getElementType();
    Constant *Undef = UndefValue::get(EltTy);
    return getConstantVector(cast<VectorType>(V->getType())->getElementType(),
                             getConstant(NumElts > 0 ? V->getOperand(0) : Undef),
                             getConstant(NumElts > 1 ? V->getOperand(1) : Undef),
                             getConstant(NumElts > 2 ? V->getOperand(2) : Undef),
                             getConstant(NumElts > 3 ? V->getOperand(3) : Undef));
  } else if (const ConstantArray *CA = dyn_cast<const ConstantArray>(CV)) {
    // handle things like [i8* bitcast (<{ i32, i32, i32 }>* @_ZTISt9bad_alloc to i8*)] which clang can emit for landingpads
    assert(CA->getNumOperands() == 1);
    CV = CA->getOperand(0);
    const ConstantExpr *CE = cast<ConstantExpr>(CV);
    CV = CE->getOperand(0); // ignore bitcast
    return getConstant(CV);
  } else if (const BlockAddress *BA = dyn_cast<const BlockAddress>(CV)) {
    return utostr(getBlockAddress(BA));
  } else if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(CV)) {
    std::string Code;
    raw_string_ostream CodeStream(Code);
    CodeStream << '(';
    generateExpression(CE, CodeStream);
    CodeStream << ')';
    return CodeStream.str();
  } else {
    CV->dump();
    llvm_unreachable("Unsupported constant kind");
  }
}

std::string JSWriter::getConstantVector(Type *ElementType, std::string x, std::string y, std::string z, std::string w) {
  // Check for a splat.
  if (x == y && x == z && x == w) {
    if (ElementType->isIntegerTy()) {
      return "SIMD_int32x4_splat(" + x + ')';
    } else {
      return "SIMD_float32x4_splat(Math_fround(" + x + "))";
    }
  }

  if (ElementType->isIntegerTy()) {
    return "SIMD_int32x4(" + x + ',' + y + ',' + z + ',' + w + ')';
  } else {
    return "SIMD_float32x4(Math_fround(" + x + "),Math_fround(" + y + "),Math_fround(" + z + "),Math_fround(" + w + "))";
  }
}

std::string JSWriter::getValueAsStr(const Value* V, AsmCast sign) {
  // Skip past no-op bitcasts and zero-index geps.
  V = V->stripPointerCasts();

  if (const Constant *CV = dyn_cast<Constant>(V)) {
    return getConstant(CV, sign);
  } else {
    return getJSName(V);
  }
}

std::string JSWriter::getValueAsCastStr(const Value* V, AsmCast sign) {
  // Skip past no-op bitcasts and zero-index geps.
  V = V->stripPointerCasts();

  if (isa<ConstantInt>(V) || isa<ConstantFP>(V)) {
    return getConstant(cast<Constant>(V), sign);
  } else {
    return getCast(getValueAsStr(V), V->getType(), sign);
  }
}

std::string JSWriter::getValueAsParenStr(const Value* V) {
  // Skip past no-op bitcasts and zero-index geps.
  V = V->stripPointerCasts();

  if (const Constant *CV = dyn_cast<Constant>(V)) {
    return getConstant(CV);
  } else {
    return "(" + getValueAsStr(V) + ")";
  }
}

std::string JSWriter::getValueAsCastParenStr(const Value* V, AsmCast sign) {
  // Skip past no-op bitcasts and zero-index geps.
  V = V->stripPointerCasts();

  if (isa<ConstantInt>(V) || isa<ConstantFP>(V) || isa<UndefValue>(V)) {
    return getConstant(cast<Constant>(V), sign);
  } else {
    return "(" + getCast(getValueAsStr(V), V->getType(), sign) + ")";
  }
}

void JSWriter::generateInsertElementExpression(const InsertElementInst *III, raw_string_ostream& Code) {
  // LLVM has no vector type constructor operator; it uses chains of
  // insertelement instructions instead. It also has no splat operator; it
  // uses an insertelement followed by a shuffle instead. If this insertelement
  // is part of either such sequence, skip it for now; we'll process it when we
  // reach the end.
  if (III->hasOneUse()) {
      const User *U = *III->user_begin();
      if (isa<InsertElementInst>(U))
          return;
      if (isa<ShuffleVectorInst>(U) &&
          isa<ConstantAggregateZero>(cast<ShuffleVectorInst>(U)->getMask()) &&
          !isa<InsertElementInst>(III->getOperand(0)) &&
          isa<ConstantInt>(III->getOperand(2)) &&
          cast<ConstantInt>(III->getOperand(2))->isZero())
      {
          return;
      }
  }

  // This insertelement is at the base of a chain of single-user insertelement
  // instructions. Collect all the inserted elements so that we can categorize
  // the chain as either a splat, a constructor, or an actual series of inserts.
  VectorType *VT = III->getType();
  unsigned NumElems = VT->getNumElements();
  unsigned NumInserted = 0;
  SmallVector<const Value *, 8> Operands(NumElems, NULL);
  const Value *Splat = III->getOperand(1);
  const Value *Base = III;
  do {
    const InsertElementInst *BaseIII = cast<InsertElementInst>(Base);
    const ConstantInt *IndexInt = cast<ConstantInt>(BaseIII->getOperand(2));
    unsigned Index = IndexInt->getZExtValue();
    if (Operands[Index] == NULL)
      ++NumInserted;
    Value *Op = BaseIII->getOperand(1);
    if (Operands[Index] == NULL) {
      Operands[Index] = Op;
      if (Op != Splat)
        Splat = NULL;
    }
    Base = BaseIII->getOperand(0);
  } while (Base->hasOneUse() && isa<InsertElementInst>(Base));

  // Emit code for the chain.
  Code << getAssignIfNeeded(III);
  if (NumInserted == NumElems) {
    if (Splat) {
      // Emit splat code.
      if (VT->getElementType()->isIntegerTy()) {
        Code << "SIMD_int32x4_splat(" << getValueAsStr(Splat) << ")";
      } else {
        std::string operand = getValueAsStr(Splat);
        if (!PreciseF32) {
          // SIMD_float32x4_splat requires an actual float32 even if we're
          // otherwise not being precise about it.
          operand = "Math_fround(" + operand + ")";
        }
        Code << "SIMD_float32x4_splat(" << operand << ")";
      }
    } else {
      // Emit constructor code.
      if (VT->getElementType()->isIntegerTy()) {
        Code << "SIMD_int32x4(";
      } else {
        Code << "SIMD_float32x4(";
      }
      for (unsigned Index = 0; Index < NumElems; ++Index) {
        if (Index != 0)
          Code << ", ";
        std::string operand = getValueAsStr(Operands[Index]);
        if (!PreciseF32 && VT->getElementType()->isFloatTy()) {
          // SIMD_float32x4_splat requires an actual float32 even if we're
          // otherwise not being precise about it.
          operand = "Math_fround(" + operand + ")";
        }
        Code << operand;
      }
      Code << ")";
    }
  } else {
    // Emit a series of inserts.
    std::string Result = getValueAsStr(Base);
    for (unsigned Index = 0; Index < NumElems; ++Index) {
      std::string with;
      if (!Operands[Index])
        continue;
      if (VT->getElementType()->isIntegerTy()) {
        with = "SIMD_int32x4_with";
      } else {
        with = "SIMD_float32x4_with";
      }
      std::string operand = getValueAsStr(Operands[Index]);
      if (!PreciseF32) {
        operand = "Math_fround(" + operand + ")";
      }
      Result = with + SIMDLane[Index] + "(" + Result + ',' + operand + ')';
    }
    Code << Result;
  }
}

void JSWriter::generateExtractElementExpression(const ExtractElementInst *EEI, raw_string_ostream& Code) {
  VectorType *VT = cast<VectorType>(EEI->getVectorOperand()->getType());
  checkVectorType(VT);
  const ConstantInt *IndexInt = dyn_cast<const ConstantInt>(EEI->getIndexOperand());
  if (IndexInt) {
    unsigned Index = IndexInt->getZExtValue();
    assert(Index <= 3);
    Code << getAssignIfNeeded(EEI);
    std::string OperandCode;
    raw_string_ostream CodeStream(OperandCode);
    CodeStream << getValueAsStr(EEI->getVectorOperand()) << '.' << simdLane[Index];
    Code << getCast(CodeStream.str(), EEI->getType());
    return;
  }

  error("SIMD extract element with non-constant index not implemented yet");
}

void JSWriter::generateShuffleVectorExpression(const ShuffleVectorInst *SVI, raw_string_ostream& Code) {
  Code << getAssignIfNeeded(SVI);

  // LLVM has no splat operator, so it makes do by using an insert and a
  // shuffle. If that's what this shuffle is doing, the code in
  // generateInsertElementExpression will have also detected it and skipped
  // emitting the insert, so we can just emit a splat here.
  if (isa<ConstantAggregateZero>(SVI->getMask()) &&
      isa<InsertElementInst>(SVI->getOperand(0)))
  {
    InsertElementInst *IEI = cast<InsertElementInst>(SVI->getOperand(0));
    if (ConstantInt *CI = dyn_cast<ConstantInt>(IEI->getOperand(2))) {
      if (CI->isZero()) {
        std::string operand = getValueAsStr(IEI->getOperand(1));
        if (!PreciseF32) {
          // SIMD_float32x4_splat requires an actual float32 even if we're
          // otherwise not being precise about it.
          operand = "Math_fround(" + operand + ")";
        }
        if (SVI->getType()->getElementType()->isIntegerTy()) {
          Code << "SIMD_int32x4_splat(";
        } else {
          Code << "SIMD_float32x4_splat(";
        }
        Code << operand << ")";
        return;
      }
    }
  }

  // Check whether can generate SIMD.js swizzle or shuffle.
  std::string A = getValueAsStr(SVI->getOperand(0));
  std::string B = getValueAsStr(SVI->getOperand(1));
  int OpNumElements = cast<VectorType>(SVI->getOperand(0)->getType())->getNumElements();
  int ResultNumElements = SVI->getType()->getNumElements();
  int Mask0 = ResultNumElements > 0 ? SVI->getMaskValue(0) : -1;
  int Mask1 = ResultNumElements > 1 ? SVI->getMaskValue(1) : -1;
  int Mask2 = ResultNumElements > 2 ? SVI->getMaskValue(2) : -1;
  int Mask3 = ResultNumElements > 3 ? SVI->getMaskValue(3) : -1;
  bool swizzleA = false;
  bool swizzleB = false;
  if ((Mask0 < OpNumElements) && (Mask1 < OpNumElements) &&
      (Mask2 < OpNumElements) && (Mask3 < OpNumElements)) {
    swizzleA = true;
  }
  if ((Mask0 < 0 || (Mask0 >= OpNumElements && Mask0 < OpNumElements * 2)) &&
      (Mask1 < 0 || (Mask1 >= OpNumElements && Mask1 < OpNumElements * 2)) &&
      (Mask2 < 0 || (Mask2 >= OpNumElements && Mask2 < OpNumElements * 2)) &&
      (Mask3 < 0 || (Mask3 >= OpNumElements && Mask3 < OpNumElements * 2))) {
    swizzleB = true;
  }
  assert(!(swizzleA && swizzleB));
  if (swizzleA || swizzleB) {
    std::string T = (swizzleA ? A : B);
    if (SVI->getType()->getElementType()->isIntegerTy()) {
      Code << "SIMD_int32x4_swizzle(" << T;
    } else {
      Code << "SIMD_float32x4_swizzle(" << T;
    }
    int i = 0;
    for (; i < ResultNumElements; ++i) {
      Code << ", ";
      int Mask = SVI->getMaskValue(i);
      if (Mask < 0) {
        Code << 0;
      } else if (Mask < OpNumElements) {
        Code << Mask;
      } else {
        assert(Mask < OpNumElements * 2);
        Code << (Mask-OpNumElements);
      }
    }
    for (; i < 4; ++i) {
      Code << ", 0";
    }
    Code << ")";
    return;
  }

  // Emit a fully-general shuffle.
  if (SVI->getType()->getElementType()->isIntegerTy()) {
    Code << "SIMD_int32x4_shuffle(";
  } else {
    Code << "SIMD_float32x4_shuffle(";
  }

  Code << A << ", " << B << ", ";

  SmallVector<int, 16> Indices;
  SVI->getShuffleMask(Indices);
  for (unsigned int i = 0; i < Indices.size(); ++i) {
    if (i != 0)
      Code << ", ";
    int Mask = Indices[i];
    if (Mask >= OpNumElements)
      Mask = Mask - OpNumElements + 4;
    if (Mask < 0)
      Code << 0;
    else
      Code << Mask;
  }

  Code << ")";
}

void JSWriter::generateICmpExpression(const ICmpInst *I, raw_string_ostream& Code) {
  bool Invert = false;
  const char *Name;
  switch (cast<ICmpInst>(I)->getPredicate()) {
    case ICmpInst::ICMP_EQ:  Name = "equal"; break;
    case ICmpInst::ICMP_NE:  Name = "equal"; Invert = true; break;
    case ICmpInst::ICMP_SLE: Name = "greaterThan"; Invert = true; break;
    case ICmpInst::ICMP_SGE: Name = "lessThan"; Invert = true; break;
    case ICmpInst::ICMP_ULE: Name = "unsignedLessThanOrEqual"; break;
    case ICmpInst::ICMP_UGE: Name = "unsignedGreaterThanOrEqual"; break;
    case ICmpInst::ICMP_ULT: Name = "unsignedLessThan"; break;
    case ICmpInst::ICMP_SLT: Name = "lessThan"; break;
    case ICmpInst::ICMP_UGT: Name = "unsignedGreaterThan"; break;
    case ICmpInst::ICMP_SGT: Name = "greaterThan"; break;
    default: I->dump(); error("invalid vector icmp"); break;
  }

  if (Invert)
    Code << "SIMD_int32x4_not(";

  Code << getAssignIfNeeded(I) << "SIMD_int32x4_" << Name << "("
       << getValueAsStr(I->getOperand(0)) << ", " << getValueAsStr(I->getOperand(1)) << ")";

  if (Invert)
    Code << ")";
}

void JSWriter::generateFCmpExpression(const FCmpInst *I, raw_string_ostream& Code) {
  const char *Name;
  bool Invert = false;
  switch (cast<FCmpInst>(I)->getPredicate()) {
    case ICmpInst::FCMP_FALSE:
      Code << "SIMD_int32x4_splat(0)";
      return;
    case ICmpInst::FCMP_TRUE:
      Code << "SIMD_int32x4_splat(-1)";
      return;
    case ICmpInst::FCMP_ONE:
      Code << "SIMD_float32x4_and(SIMD_float32x4_and("
              "SIMD_float32x4_equal(" << getValueAsStr(I->getOperand(0)) << ", "
                                      << getValueAsStr(I->getOperand(0)) << "), " <<
              "SIMD_float32x4_equal(" << getValueAsStr(I->getOperand(1)) << ", "
                                      << getValueAsStr(I->getOperand(1)) << ")), " <<
              "SIMD_float32x4_notEqual(" << getValueAsStr(I->getOperand(0)) << ", "
                                         << getValueAsStr(I->getOperand(1)) << "))";
      return;
    case ICmpInst::FCMP_UEQ:
      Code << "SIMD_float32x4_or(SIMD_float32x4_or("
              "SIMD_float32x4_notEqual(" << getValueAsStr(I->getOperand(0)) << ", "
                                         << getValueAsStr(I->getOperand(0)) << "), " <<
              "SIMD_float32x4_notEqual(" << getValueAsStr(I->getOperand(1)) << ", "
                                         << getValueAsStr(I->getOperand(1)) << ")), " <<
              "SIMD_float32x4_equal(" << getValueAsStr(I->getOperand(0)) << ", "
                                      << getValueAsStr(I->getOperand(1)) << "))";
      return;
    case FCmpInst::FCMP_ORD:
      Code << "SIMD_float32x4_and("
              "SIMD_float32x4_equal(" << getValueAsStr(I->getOperand(0)) << ", " << getValueAsStr(I->getOperand(0)) << "), " <<
              "SIMD_float32x4_equal(" << getValueAsStr(I->getOperand(1)) << ", " << getValueAsStr(I->getOperand(1)) << "))";
      return;

    case FCmpInst::FCMP_UNO:
      Code << "SIMD_float32x4_or("
              "SIMD_float32x4_notEqual(" << getValueAsStr(I->getOperand(0)) << ", " << getValueAsStr(I->getOperand(0)) << "), " <<
              "SIMD_float32x4_notEqual(" << getValueAsStr(I->getOperand(1)) << ", " << getValueAsStr(I->getOperand(1)) << "))";
      return;

    case ICmpInst::FCMP_OEQ:  Name = "equal"; break;
    case ICmpInst::FCMP_OGT:  Name = "greaterThan"; break;
    case ICmpInst::FCMP_OGE:  Name = "greaterThanOrEqual"; break;
    case ICmpInst::FCMP_OLT:  Name = "lessThan"; break;
    case ICmpInst::FCMP_OLE:  Name = "lessThanOrEqual"; break;
    case ICmpInst::FCMP_UGT:  Name = "lessThanOrEqual"; Invert = true; break;
    case ICmpInst::FCMP_UGE:  Name = "lessThan"; Invert = true; break;
    case ICmpInst::FCMP_ULT:  Name = "greaterThanOrEqual"; Invert = true; break;
    case ICmpInst::FCMP_ULE:  Name = "greaterThan"; Invert = true; break;
    case ICmpInst::FCMP_UNE:  Name = "notEqual"; break;
    default: I->dump(); error("invalid vector fcmp"); break;
  }

  if (Invert)
    Code << "SIMD_int32x4_not(";

  Code << getAssignIfNeeded(I) << "SIMD_float32x4_" << Name << "("
       << getValueAsStr(I->getOperand(0)) << ", " << getValueAsStr(I->getOperand(1)) << ")";

  if (Invert)
    Code << ")";
}

static const Value *getElement(const Value *V, unsigned i) {
    if (const InsertElementInst *II = dyn_cast<InsertElementInst>(V)) {
        if (ConstantInt *CI = dyn_cast<ConstantInt>(II->getOperand(2))) {
            if (CI->equalsInt(i))
                return II->getOperand(1);
        }
        return getElement(II->getOperand(0), i);
    }
    return NULL;
}

static const Value *getSplatValue(const Value *V) {
    if (const Constant *C = dyn_cast<Constant>(V))
        return C->getSplatValue();

    VectorType *VTy = cast<VectorType>(V->getType());
    const Value *Result = NULL;
    for (unsigned i = 0; i < VTy->getNumElements(); ++i) {
        const Value *E = getElement(V, i);
        if (!E)
            return NULL;
        if (!Result)
            Result = E;
        else if (Result != E)
            return NULL;
    }
    return Result;

}

void JSWriter::generateShiftExpression(const BinaryOperator *I, raw_string_ostream& Code) {
    // If we're shifting every lane by the same amount (shifting by a splat value
    // then we can use a ByScalar shift.
    const Value *Count = I->getOperand(1);
    if (const Value *Splat = getSplatValue(Count)) {
        Code << getAssignIfNeeded(I) << "SIMD_int32x4_";
        if (I->getOpcode() == Instruction::AShr)
            Code << "shiftRightArithmeticByScalar";
        else if (I->getOpcode() == Instruction::LShr)
            Code << "shiftRightLogicalByScalar";
        else
            Code << "shiftLeftByScalar";
        Code << "(" << getValueAsStr(I->getOperand(0)) << ", " << getValueAsStr(Splat) << ")";
        return;
    }

    // SIMD.js does not currently have vector-vector shifts.
    generateUnrolledExpression(I, Code);
}

void JSWriter::generateUnrolledExpression(const User *I, raw_string_ostream& Code) {
  VectorType *VT = cast<VectorType>(I->getType());

  Code << getAssignIfNeeded(I);

  if (VT->getElementType()->isIntegerTy()) {
    Code << "SIMD_int32x4(";
  } else {
    Code << "SIMD_float32x4(";
  }

  for (unsigned Index = 0; Index < VT->getNumElements(); ++Index) {
    if (Index != 0)
        Code << ", ";
    if (!PreciseF32 && VT->getElementType()->isFloatTy()) {
        Code << "Math_fround(";
    }
    std::string Lane = VT->getNumElements() <= 4 ?
                       std::string(".") + simdLane[Index] :
                       ".s" + utostr(Index);
    switch (Operator::getOpcode(I)) {
      case Instruction::SDiv:
        Code << "(" << getValueAsStr(I->getOperand(0)) << Lane << "|0) / ("
             << getValueAsStr(I->getOperand(1)) << Lane << "|0)|0";
        break;
      case Instruction::UDiv:
        Code << "(" << getValueAsStr(I->getOperand(0)) << Lane << ">>>0) / ("
             << getValueAsStr(I->getOperand(1)) << Lane << ">>>0)>>>0";
        break;
      case Instruction::SRem:
        Code << "(" << getValueAsStr(I->getOperand(0)) << Lane << "|0) / ("
             << getValueAsStr(I->getOperand(1)) << Lane << "|0)|0";
        break;
      case Instruction::URem:
        Code << "(" << getValueAsStr(I->getOperand(0)) << Lane << ">>>0) / ("
             << getValueAsStr(I->getOperand(1)) << Lane << ">>>0)>>>0";
        break;
      case Instruction::AShr:
        Code << "(" << getValueAsStr(I->getOperand(0)) << Lane << "|0) >> ("
             << getValueAsStr(I->getOperand(1)) << Lane << "|0)|0";
        break;
      case Instruction::LShr:
        Code << "(" << getValueAsStr(I->getOperand(0)) << Lane << "|0) >>> ("
             << getValueAsStr(I->getOperand(1)) << Lane << "|0)|0";
        break;
      case Instruction::Shl:
        Code << "(" << getValueAsStr(I->getOperand(0)) << Lane << "|0) << ("
             << getValueAsStr(I->getOperand(1)) << Lane << "|0)|0";
        break;
      default: I->dump(); error("invalid unrolled vector instr"); break;
    }
    if (!PreciseF32 && VT->getElementType()->isFloatTy()) {
        Code << ")";
    }
  }

  Code << ")";
}

bool JSWriter::generateSIMDExpression(const User *I, raw_string_ostream& Code) {
  VectorType *VT;
  if ((VT = dyn_cast<VectorType>(I->getType()))) {
    // vector-producing instructions
    checkVectorType(VT);

    switch (Operator::getOpcode(I)) {
      default: I->dump(); error("invalid vector instr"); break;
      case Instruction::Call: // return value is just a SIMD value, no special handling
        return false;
      case Instruction::PHI: // handled separately - we push them back into the relooper branchings
        break;
      case Instruction::ICmp:
        generateICmpExpression(cast<ICmpInst>(I), Code);
        break;
      case Instruction::FCmp:
        generateFCmpExpression(cast<FCmpInst>(I), Code);
        break;
      case Instruction::SExt:
        assert(cast<VectorType>(I->getOperand(0)->getType())->getElementType()->isIntegerTy(1) &&
               "sign-extension from vector of other than i1 not yet supported");
        // Since we represent vectors of i1 as vectors of sign extended wider integers,
        // sign extending them is a no-op.
        Code << getAssignIfNeeded(I) << getValueAsStr(I->getOperand(0));
        break;
      case Instruction::Select:
        // Since we represent vectors of i1 as vectors of sign extended wider integers,
        // selecting on them is just an elementwise select.
        if (isa<VectorType>(I->getOperand(0)->getType())) {
          if (cast<VectorType>(I->getType())->getElementType()->isIntegerTy()) {
            Code << getAssignIfNeeded(I) << "SIMD_int32x4_select(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << "," << getValueAsStr(I->getOperand(2)) << ")"; break;
          } else {
            Code << getAssignIfNeeded(I) << "SIMD_float32x4_select(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << "," << getValueAsStr(I->getOperand(2)) << ")"; break;
          }
          return true;
        }
        // Otherwise we have a scalar condition, so it's a ?: operator.
        return false;
      case Instruction::FAdd: Code << getAssignIfNeeded(I) << "SIMD_float32x4_add(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")"; break;
      case Instruction::FMul: Code << getAssignIfNeeded(I) << "SIMD_float32x4_mul(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")"; break;
      case Instruction::FDiv: Code << getAssignIfNeeded(I) << "SIMD_float32x4_div(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")"; break;
      case Instruction::Add: Code << getAssignIfNeeded(I) << "SIMD_int32x4_add(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")"; break;
      case Instruction::Sub: Code << getAssignIfNeeded(I) << "SIMD_int32x4_sub(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")"; break;
      case Instruction::Mul: Code << getAssignIfNeeded(I) << "SIMD_int32x4_mul(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")"; break;
      case Instruction::And: Code << getAssignIfNeeded(I) << "SIMD_int32x4_and(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")"; break;
      case Instruction::Or:  Code << getAssignIfNeeded(I) << "SIMD_int32x4_or(" <<  getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")"; break;
      case Instruction::Xor:
        // LLVM represents a not(x) as -1 ^ x
        Code << getAssignIfNeeded(I);
        if (BinaryOperator::isNot(I)) {
          Code << "SIMD_int32x4_not(" << getValueAsStr(BinaryOperator::getNotArgument(I)) << ")"; break;
        } else {
          Code << "SIMD_int32x4_xor(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")"; break;
        }
        break;
      case Instruction::FSub:
        // LLVM represents an fneg(x) as -0.0 - x.
        Code << getAssignIfNeeded(I);
        if (BinaryOperator::isFNeg(I)) {
          Code << "SIMD_float32x4_neg(" << getValueAsStr(BinaryOperator::getFNegArgument(I)) << ")";
        } else {
          Code << "SIMD_float32x4_sub(" << getValueAsStr(I->getOperand(0)) << "," << getValueAsStr(I->getOperand(1)) << ")";
        }
        break;
      case Instruction::BitCast: {
        Code << getAssignIfNeeded(I);
        if (cast<VectorType>(I->getType())->getElementType()->isIntegerTy()) {
          Code << "SIMD_int32x4_fromFloat32x4Bits(" << getValueAsStr(I->getOperand(0)) << ')';
        } else {
          Code << "SIMD_float32x4_fromInt32x4Bits(" << getValueAsStr(I->getOperand(0)) << ')';
        }
        break;
      }
      case Instruction::Load: {
        const LoadInst *LI = cast<LoadInst>(I);
        const Value *P = LI->getPointerOperand();
        std::string PS = getValueAsStr(P);

        // Determine if this is a partial load.
        static const std::string partialAccess[4] = { "X", "XY", "XYZ", "" };
        if (VT->getNumElements() < 1 || VT->getNumElements() > 4) {
          error("invalid number of lanes in SIMD operation!");
          break;
        }
        const std::string &Part = partialAccess[VT->getNumElements() - 1];

        Code << getAssignIfNeeded(I);
        if (VT->getElementType()->isIntegerTy()) {
          Code << "SIMD_int32x4_load" << Part << "(HEAPU8, " << PS << ")";
        } else {
          Code << "SIMD_float32x4_load" << Part << "(HEAPU8, " << PS << ")";
        }
        break;
      }
      case Instruction::InsertElement:
        generateInsertElementExpression(cast<InsertElementInst>(I), Code);
        break;
      case Instruction::ShuffleVector:
        generateShuffleVectorExpression(cast<ShuffleVectorInst>(I), Code);
        break;
      case Instruction::SDiv:
      case Instruction::UDiv:
      case Instruction::SRem:
      case Instruction::URem:
        // The SIMD API does not currently support these operations directly.
        // Emulate them using scalar operations (which is essentially the same
        // as what would happen if the API did support them, since hardware
        // doesn't support them).
        generateUnrolledExpression(I, Code);
        break;
      case Instruction::AShr:
      case Instruction::LShr:
      case Instruction::Shl:
        generateShiftExpression(cast<BinaryOperator>(I), Code);
        break;
    }
    return true;
  } else {
    // vector-consuming instructions
    if (Operator::getOpcode(I) == Instruction::Store && (VT = dyn_cast<VectorType>(I->getOperand(0)->getType())) && VT->isVectorTy()) {
      checkVectorType(VT);
      const StoreInst *SI = cast<StoreInst>(I);
      const Value *P = SI->getPointerOperand();
      std::string PS = getOpName(P);
      std::string VS = getValueAsStr(SI->getValueOperand());
      Code << getAdHocAssign(PS, P->getType()) << getValueAsStr(P) << ';';

      // Determine if this is a partial store.
      static const std::string partialAccess[4] = { "X", "XY", "XYZ", "" };
      if (VT->getNumElements() < 1 || VT->getNumElements() > 4) {
        error("invalid number of lanes in SIMD operation!");
        return false;
      }
      const std::string &Part = partialAccess[VT->getNumElements() - 1];

      if (VT->getElementType()->isIntegerTy()) {
        Code << "SIMD_int32x4_store" << Part << "(HEAPU8, " << PS << ", " << VS << ")";
      } else {
        Code << "SIMD_float32x4_store" << Part << "(HEAPU8, " << PS << ", " << VS << ")";
      }
      return true;
    } else if (Operator::getOpcode(I) == Instruction::ExtractElement) {
      generateExtractElementExpression(cast<ExtractElementInst>(I), Code);
      return true;
    }
  }
  return false;
}

static uint64_t LSBMask(unsigned numBits) {
  return numBits >= 64 ? 0xFFFFFFFFFFFFFFFFULL : (1ULL << numBits) - 1;
}

// Generate code for and operator, either an Instruction or a ConstantExpr.
void JSWriter::generateExpression(const User *I, raw_string_ostream& Code) {
  // To avoid emiting code and variables for the no-op pointer bitcasts
  // and all-zero-index geps that LLVM needs to satisfy its type system, we
  // call stripPointerCasts() on all values before translating them. This
  // includes bitcasts whose only use is lifetime marker intrinsics.
  assert(I == I->stripPointerCasts());

  Type *T = I->getType();
  if (T->isIntegerTy() && T->getIntegerBitWidth() > 32) {
    errs() << *I << "\n";
    report_fatal_error("legalization problem");
  }

  if (!generateSIMDExpression(I, Code)) switch (Operator::getOpcode(I)) {
  default: {
    I->dump();
    error("Invalid instruction");
    break;
  }
  case Instruction::Ret: {
    const ReturnInst* ret =  cast<ReturnInst>(I);
    const Value *RV = ret->getReturnValue();
    if (StackBumped) {
      Code << "STACKTOP = sp;";
    }
    Code << "return";
    if (RV != NULL) {
      Code << " " << getValueAsCastParenStr(RV, ASM_NONSPECIFIC | ASM_MUST_CAST);
    }
    break;
  }
  case Instruction::Br:
  case Instruction::IndirectBr:
  case Instruction::Switch: return; // handled while relooping
  case Instruction::Unreachable: {
    // Typically there should be an abort right before these, so we don't emit any code // TODO: when ASSERTIONS are on, emit abort(0)
    Code << "// unreachable";
    break;
  }
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction::FSub:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::FRem:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:{
    Code << getAssignIfNeeded(I);
    unsigned opcode = Operator::getOpcode(I);
    switch (opcode) {
      case Instruction::Add:  Code << getParenCast(
                                        getValueAsParenStr(I->getOperand(0)) +
                                        " + " +
                                        getValueAsParenStr(I->getOperand(1)),
                                        I->getType()
                                      ); break;
      case Instruction::Sub:  Code << getParenCast(
                                        getValueAsParenStr(I->getOperand(0)) +
                                        " - " +
                                        getValueAsParenStr(I->getOperand(1)),
                                        I->getType()
                                      ); break;
      case Instruction::Mul:  Code << getIMul(I->getOperand(0), I->getOperand(1)); break;
      case Instruction::UDiv:
      case Instruction::SDiv:
      case Instruction::URem:
      case Instruction::SRem: Code << "(" <<
                                      getValueAsCastParenStr(I->getOperand(0), (opcode == Instruction::SDiv || opcode == Instruction::SRem) ? ASM_SIGNED : ASM_UNSIGNED) <<
                                      ((opcode == Instruction::UDiv || opcode == Instruction::SDiv) ? " / " : " % ") <<
                                      getValueAsCastParenStr(I->getOperand(1), (opcode == Instruction::SDiv || opcode == Instruction::SRem) ? ASM_SIGNED : ASM_UNSIGNED) <<
                                      ")&-1"; break;
      case Instruction::And:  Code << getValueAsStr(I->getOperand(0)) << " & " <<   getValueAsStr(I->getOperand(1)); break;
      case Instruction::Or:   Code << getValueAsStr(I->getOperand(0)) << " | " <<   getValueAsStr(I->getOperand(1)); break;
      case Instruction::Xor:  Code << getValueAsStr(I->getOperand(0)) << " ^ " <<   getValueAsStr(I->getOperand(1)); break;
      case Instruction::Shl:  {
        std::string Shifted = getValueAsStr(I->getOperand(0)) + " << " +  getValueAsStr(I->getOperand(1));
        if (I->getType()->getIntegerBitWidth() < 32) {
          Shifted = getParenCast(Shifted, I->getType(), ASM_UNSIGNED); // remove bits that are shifted beyond the size of this value
        }
        Code << Shifted;
        break;
      }
      case Instruction::AShr:
      case Instruction::LShr: {
        std::string Input = getValueAsStr(I->getOperand(0));
        if (I->getType()->getIntegerBitWidth() < 32) {
          Input = '(' + getCast(Input, I->getType(), opcode == Instruction::AShr ? ASM_SIGNED : ASM_UNSIGNED) + ')'; // fill in high bits, as shift needs those and is done in 32-bit
        }
        Code << Input << (opcode == Instruction::AShr ? " >> " : " >>> ") <<  getValueAsStr(I->getOperand(1));
        break;
      }

      case Instruction::FAdd: Code << ensureFloat(getValueAsStr(I->getOperand(0)) + " + " + getValueAsStr(I->getOperand(1)), I->getType()); break;
      case Instruction::FMul: Code << ensureFloat(getValueAsStr(I->getOperand(0)) + " * " + getValueAsStr(I->getOperand(1)), I->getType()); break;
      case Instruction::FDiv: Code << ensureFloat(getValueAsStr(I->getOperand(0)) + " / " + getValueAsStr(I->getOperand(1)), I->getType()); break;
      case Instruction::FRem: Code << ensureFloat(getValueAsStr(I->getOperand(0)) + " % " + getValueAsStr(I->getOperand(1)), I->getType()); break;
      case Instruction::FSub:
        // LLVM represents an fneg(x) as -0.0 - x.
        if (BinaryOperator::isFNeg(I)) {
          Code << ensureFloat("-" + getValueAsStr(BinaryOperator::getFNegArgument(I)), I->getType());
        } else {
          Code << ensureFloat(getValueAsStr(I->getOperand(0)) + " - " + getValueAsStr(I->getOperand(1)), I->getType());
        }
        break;
      default: error("bad binary opcode"); break;
    }
    break;
  }
  case Instruction::FCmp: {
    Code << getAssignIfNeeded(I);
    switch (cast<FCmpInst>(I)->getPredicate()) {
      // Comparisons which are simple JS operators.
      case FCmpInst::FCMP_OEQ:   Code << getValueAsStr(I->getOperand(0)) << " == " << getValueAsStr(I->getOperand(1)); break;
      case FCmpInst::FCMP_UNE:   Code << getValueAsStr(I->getOperand(0)) << " != " << getValueAsStr(I->getOperand(1)); break;
      case FCmpInst::FCMP_OGT:   Code << getValueAsStr(I->getOperand(0)) << " > "  << getValueAsStr(I->getOperand(1)); break;
      case FCmpInst::FCMP_OGE:   Code << getValueAsStr(I->getOperand(0)) << " >= " << getValueAsStr(I->getOperand(1)); break;
      case FCmpInst::FCMP_OLT:   Code << getValueAsStr(I->getOperand(0)) << " < "  << getValueAsStr(I->getOperand(1)); break;
      case FCmpInst::FCMP_OLE:   Code << getValueAsStr(I->getOperand(0)) << " <= " << getValueAsStr(I->getOperand(1)); break;

      // Comparisons which are inverses of JS operators.
      case FCmpInst::FCMP_UGT:
        Code << "!(" << getValueAsStr(I->getOperand(0)) << " <= " << getValueAsStr(I->getOperand(1)) << ")";
        break;
      case FCmpInst::FCMP_UGE:
        Code << "!(" << getValueAsStr(I->getOperand(0)) << " < "  << getValueAsStr(I->getOperand(1)) << ")";
        break;
      case FCmpInst::FCMP_ULT:
        Code << "!(" << getValueAsStr(I->getOperand(0)) << " >= " << getValueAsStr(I->getOperand(1)) << ")";
        break;
      case FCmpInst::FCMP_ULE:
        Code << "!(" << getValueAsStr(I->getOperand(0)) << " > "  << getValueAsStr(I->getOperand(1)) << ")";
        break;

      // Comparisons which require explicit NaN checks.
      case FCmpInst::FCMP_UEQ:
        Code << "(" << getValueAsStr(I->getOperand(0)) << " != " << getValueAsStr(I->getOperand(0)) << ") | " <<
                "(" << getValueAsStr(I->getOperand(1)) << " != " << getValueAsStr(I->getOperand(1)) << ") |" <<
                "(" << getValueAsStr(I->getOperand(0)) << " == " << getValueAsStr(I->getOperand(1)) << ")";
        break;
      case FCmpInst::FCMP_ONE:
        Code << "(" << getValueAsStr(I->getOperand(0)) << " == " << getValueAsStr(I->getOperand(0)) << ") & " <<
                "(" << getValueAsStr(I->getOperand(1)) << " == " << getValueAsStr(I->getOperand(1)) << ") &" <<
                "(" << getValueAsStr(I->getOperand(0)) << " != " << getValueAsStr(I->getOperand(1)) << ")";
        break;

      // Simple NaN checks.
      case FCmpInst::FCMP_ORD:   Code << "(" << getValueAsStr(I->getOperand(0)) << " == " << getValueAsStr(I->getOperand(0)) << ") & " <<
                                         "(" << getValueAsStr(I->getOperand(1)) << " == " << getValueAsStr(I->getOperand(1)) << ")"; break;
      case FCmpInst::FCMP_UNO:   Code << "(" << getValueAsStr(I->getOperand(0)) << " != " << getValueAsStr(I->getOperand(0)) << ") | " <<
                                         "(" << getValueAsStr(I->getOperand(1)) << " != " << getValueAsStr(I->getOperand(1)) << ")"; break;

      // Simple constants.
      case FCmpInst::FCMP_FALSE: Code << "0"; break;
      case FCmpInst::FCMP_TRUE : Code << "1"; break;

      default: error("bad fcmp"); break;
    }
    break;
  }
  case Instruction::ICmp: {
    unsigned predicate = isa<ConstantExpr>(I) ?
                         cast<ConstantExpr>(I)->getPredicate() :
                         cast<ICmpInst>(I)->getPredicate();
    AsmCast sign = CmpInst::isUnsigned(predicate) ? ASM_UNSIGNED : ASM_SIGNED;
    Code << getAssignIfNeeded(I) << "(" <<
      getValueAsCastStr(I->getOperand(0), sign) <<
    ")";
    switch (predicate) {
    case ICmpInst::ICMP_EQ:  Code << "==";  break;
    case ICmpInst::ICMP_NE:  Code << "!=";  break;
    case ICmpInst::ICMP_ULE: Code << "<="; break;
    case ICmpInst::ICMP_SLE: Code << "<="; break;
    case ICmpInst::ICMP_UGE: Code << ">="; break;
    case ICmpInst::ICMP_SGE: Code << ">="; break;
    case ICmpInst::ICMP_ULT: Code << "<"; break;
    case ICmpInst::ICMP_SLT: Code << "<"; break;
    case ICmpInst::ICMP_UGT: Code << ">"; break;
    case ICmpInst::ICMP_SGT: Code << ">"; break;
    default: llvm_unreachable("Invalid ICmp predicate");
    }
    Code << "(" <<
      getValueAsCastStr(I->getOperand(1), sign) <<
    ")";
    break;
  }
  case Instruction::Alloca: {
    const AllocaInst* AI = cast<AllocaInst>(I);

    // We've done an alloca, so we'll have bumped the stack and will
    // need to restore it.
    // Yes, we shouldn't have to bump it for nativized vars, however
    // they are included in the frame offset, so the restore is still
    // needed until that is fixed.
    StackBumped = true;

    if (NativizedVars.count(AI)) {
      // nativized stack variable, we just need a 'var' definition
      UsedVars[getJSName(AI)] = AI->getType()->getElementType();
      return;
    }

    // Fixed-size entry-block allocations are allocated all at once in the
    // function prologue.
    if (AI->isStaticAlloca()) {
      uint64_t Offset;
      if (Allocas.getFrameOffset(AI, &Offset)) {
        Code << getAssign(AI);
        if (Allocas.getMaxAlignment() <= STACK_ALIGN) {
          Code << "sp";
        } else {
          Code << "sp_a"; // aligned base of stack is different, use that
        }
        if (Offset != 0) {
          Code << " + " << Offset << "|0";
        }
        break;
      }
      // Otherwise, this alloca is being represented by another alloca, so
      // there's nothing to print.
      return;
    }

    assert(AI->getAlignment() <= STACK_ALIGN); // TODO

    Type *T = AI->getAllocatedType();
    std::string Size;
    uint64_t BaseSize = DL->getTypeAllocSize(T);
    const Value *AS = AI->getArraySize();
    if (const ConstantInt *CI = dyn_cast<ConstantInt>(AS)) {
      Size = Twine(stackAlign(BaseSize * CI->getZExtValue())).str();
    } else {
      Size = stackAlignStr("((" + utostr(BaseSize) + '*' + getValueAsStr(AS) + ")|0)");
    }
    Code << getAssign(AI) << "STACKTOP; " << getStackBump(Size);
    break;
  }
  case Instruction::Load: {
    const LoadInst *LI = cast<LoadInst>(I);
    const Value *P = LI->getPointerOperand();
    unsigned Alignment = LI->getAlignment();
    if (NativizedVars.count(P)) {
      Code << getAssign(LI) << getValueAsStr(P);
    } else {
      Code << getLoad(LI, P, LI->getType(), Alignment);
    }
    break;
  }
  case Instruction::Store: {
    const StoreInst *SI = cast<StoreInst>(I);
    const Value *P = SI->getPointerOperand();
    const Value *V = SI->getValueOperand();
    unsigned Alignment = SI->getAlignment();
    std::string VS = getValueAsStr(V);
    if (NativizedVars.count(P)) {
      Code << getValueAsStr(P) << " = " << VS;
    } else {
      Code << getStore(SI, P, V->getType(), VS, Alignment);
    }

    Type *T = V->getType();
    if (T->isIntegerTy() && T->getIntegerBitWidth() > 32) {
      errs() << *I << "\n";
      report_fatal_error("legalization problem");
    }
    break;
  }
  case Instruction::GetElementPtr: {
    Code << getAssignIfNeeded(I);
    const GEPOperator *GEP = cast<GEPOperator>(I);
    gep_type_iterator GTI = gep_type_begin(GEP);
    int32_t ConstantOffset = 0;
    std::string text = getValueAsParenStr(GEP->getPointerOperand());

    GetElementPtrInst::const_op_iterator I = GEP->op_begin();
    I++;
    for (GetElementPtrInst::const_op_iterator E = GEP->op_end();
       I != E; ++I) {
      const Value *Index = *I;
      if (StructType *STy = dyn_cast<StructType>(*GTI++)) {
        // For a struct, add the member offset.
        unsigned FieldNo = cast<ConstantInt>(Index)->getZExtValue();
        uint32_t Offset = DL->getStructLayout(STy)->getElementOffset(FieldNo);
        ConstantOffset = (uint32_t)ConstantOffset + Offset;
      } else {
        // For an array, add the element offset, explicitly scaled.
        uint32_t ElementSize = DL->getTypeAllocSize(*GTI);
        if (const ConstantInt *CI = dyn_cast<ConstantInt>(Index)) {
          ConstantOffset = (uint32_t)ConstantOffset + (uint32_t)CI->getSExtValue() * ElementSize;
        } else {
          text = "(" + text + " + (" + getIMul(Index, ConstantInt::get(Type::getInt32Ty(GEP->getContext()), ElementSize)) + ")|0)";
        }
      }
    }
    if (ConstantOffset != 0) {
      text = "(" + text + " + " + itostr(ConstantOffset) + "|0)";
    }
    Code << text;
    break;
  }
  case Instruction::PHI: {
    // handled separately - we push them back into the relooper branchings
    return;
  }
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
    Code << getAssignIfNeeded(I) << getValueAsStr(I->getOperand(0));
    break;
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::SIToFP: {
    Code << getAssignIfNeeded(I);
    switch (Operator::getOpcode(I)) {
    case Instruction::Trunc: {
      //unsigned inBits = V->getType()->getIntegerBitWidth();
      unsigned outBits = I->getType()->getIntegerBitWidth();
      Code << getValueAsStr(I->getOperand(0)) << "&" << utostr(LSBMask(outBits));
      break;
    }
    case Instruction::SExt: {
      std::string bits = utostr(32 - I->getOperand(0)->getType()->getIntegerBitWidth());
      Code << getValueAsStr(I->getOperand(0)) << " << " << bits << " >> " << bits;
      break;
    }
    case Instruction::ZExt: {
      Code << getValueAsCastStr(I->getOperand(0), ASM_UNSIGNED);
      break;
    }
    case Instruction::FPExt: {
      if (PreciseF32) {
        Code << "+" << getValueAsStr(I->getOperand(0)); break;
      } else {
        Code << getValueAsStr(I->getOperand(0)); break;
      }
      break;
    }
    case Instruction::FPTrunc: {
      Code << ensureFloat(getValueAsStr(I->getOperand(0)), I->getType());
      break;
    }
    case Instruction::SIToFP:   Code << '(' << getCast(getValueAsCastParenStr(I->getOperand(0), ASM_SIGNED),   I->getType()) << ')'; break;
    case Instruction::UIToFP:   Code << '(' << getCast(getValueAsCastParenStr(I->getOperand(0), ASM_UNSIGNED), I->getType()) << ')'; break;
    case Instruction::FPToSI:   Code << '(' << getDoubleToInt(getValueAsParenStr(I->getOperand(0))) << ')'; break;
    case Instruction::FPToUI:   Code << '(' << getCast(getDoubleToInt(getValueAsParenStr(I->getOperand(0))), I->getType(), ASM_UNSIGNED) << ')'; break;
    case Instruction::PtrToInt: Code << '(' << getValueAsStr(I->getOperand(0)) << ')'; break;
    case Instruction::IntToPtr: Code << '(' << getValueAsStr(I->getOperand(0)) << ')'; break;
    default: llvm_unreachable("Unreachable");
    }
    break;
  }
  case Instruction::BitCast: {
    Code << getAssignIfNeeded(I);
    // Most bitcasts are no-ops for us. However, the exception is int to float and float to int
    Type *InType = I->getOperand(0)->getType();
    Type *OutType = I->getType();
    std::string V = getValueAsStr(I->getOperand(0));
    if (InType->isIntegerTy() && OutType->isFloatingPointTy()) {
      assert(InType->getIntegerBitWidth() == 32);
      Code << "(HEAP32[tempDoublePtr>>2]=" << V << "," << getCast("HEAPF32[tempDoublePtr>>2]", Type::getFloatTy(TheModule->getContext())) << ")";
    } else if (OutType->isIntegerTy() && InType->isFloatingPointTy()) {
      assert(OutType->getIntegerBitWidth() == 32);
      Code << "(HEAPF32[tempDoublePtr>>2]=" << V << "," "HEAP32[tempDoublePtr>>2]|0)";
    } else {
      Code << V;
    }
    break;
  }
  case Instruction::Call: {
    const CallInst *CI = cast<CallInst>(I);
    std::string Call = handleCall(CI);
    if (Call.empty()) return;
    Code << Call;
    break;
  }
  case Instruction::Select: {
    Code << getAssignIfNeeded(I) << getValueAsStr(I->getOperand(0)) << " ? " <<
                                    getValueAsStr(I->getOperand(1)) << " : " <<
                                    getValueAsStr(I->getOperand(2));
    break;
  }
  case Instruction::AtomicRMW: {
    const AtomicRMWInst *rmwi = cast<AtomicRMWInst>(I);
    const Value *P = rmwi->getOperand(0);
    const Value *V = rmwi->getOperand(1);
    std::string VS = getValueAsStr(V);
    Code << getLoad(rmwi, P, I->getType(), 0) << ';';
    // Most bitcasts are no-ops for us. However, the exception is int to float and float to int
    switch (rmwi->getOperation()) {
      case AtomicRMWInst::Xchg: Code << getStore(rmwi, P, I->getType(), VS, 0); break;
      case AtomicRMWInst::Add:  Code << getStore(rmwi, P, I->getType(), "((" + getJSName(I) + '+' + VS + ")|0)", 0); break;
      case AtomicRMWInst::Sub:  Code << getStore(rmwi, P, I->getType(), "((" + getJSName(I) + '-' + VS + ")|0)", 0); break;
      case AtomicRMWInst::And:  Code << getStore(rmwi, P, I->getType(), "(" + getJSName(I) + '&' + VS + ")", 0); break;
      case AtomicRMWInst::Nand: Code << getStore(rmwi, P, I->getType(), "(~(" + getJSName(I) + '&' + VS + "))", 0); break;
      case AtomicRMWInst::Or:   Code << getStore(rmwi, P, I->getType(), "(" + getJSName(I) + '|' + VS + ")", 0); break;
      case AtomicRMWInst::Xor:  Code << getStore(rmwi, P, I->getType(), "(" + getJSName(I) + '^' + VS + ")", 0); break;
      case AtomicRMWInst::Max:
      case AtomicRMWInst::Min:
      case AtomicRMWInst::UMax:
      case AtomicRMWInst::UMin:
      case AtomicRMWInst::BAD_BINOP: llvm_unreachable("Bad atomic operation");
    }
    break;
  }
  case Instruction::Fence: // no threads, so nothing to do here
    Code << "/* fence */";
    break;
  }

  if (const Instruction *Inst = dyn_cast<Instruction>(I)) {
    Code << ';';
    // append debug info
    emitDebugInfo(Code, Inst);
    Code << '\n';
  }
}

// Checks whether to use a condition variable. We do so for switches and for indirectbrs
static const Value *considerConditionVar(const Instruction *I) {
  if (const IndirectBrInst *IB = dyn_cast<const IndirectBrInst>(I)) {
    return IB->getAddress();
  }
  const SwitchInst *SI = dyn_cast<SwitchInst>(I);
  if (!SI) return NULL;
  // use a switch if the range is not too big or sparse
  int64_t Minn = INT64_MAX, Maxx = INT64_MIN;
  for (SwitchInst::ConstCaseIt i = SI->case_begin(), e = SI->case_end(); i != e; ++i) {
    int64_t Curr = i.getCaseValue()->getSExtValue();
    if (Curr < Minn) Minn = Curr;
    if (Curr > Maxx) Maxx = Curr;
  }
  int64_t Range = Maxx - Minn;
  int Num = SI->getNumCases();
  return Num < 5 || Range > 10*1024 || (Range/Num) > 1024 ? NULL : SI->getCondition(); // heuristics
}

void JSWriter::addBlock(const BasicBlock *BB, Relooper& R, LLVMToRelooperMap& LLVMToRelooper) {
  std::string Code;
  raw_string_ostream CodeStream(Code);
  for (BasicBlock::const_iterator I = BB->begin(), E = BB->end();
       I != E; ++I) {
    if (I->stripPointerCasts() == I) {
      generateExpression(I, CodeStream);
    }
  }
  CodeStream.flush();
  const Value* Condition = considerConditionVar(BB->getTerminator());
  Block *Curr = new Block(Code.c_str(), Condition ? getValueAsCastStr(Condition).c_str() : NULL);
  LLVMToRelooper[BB] = Curr;
  R.AddBlock(Curr);
}

void JSWriter::printFunctionBody(const Function *F) {
  assert(!F->isDeclaration());

  // Prepare relooper
  Relooper::MakeOutputBuffer(1024*1024);
  Relooper R;
  //if (!canReloop(F)) R.SetEmulate(true);
  if (F->getAttributes().hasAttribute(AttributeSet::FunctionIndex, Attribute::MinSize) ||
      F->getAttributes().hasAttribute(AttributeSet::FunctionIndex, Attribute::OptimizeForSize)) {
    R.SetMinSize(true);
  }
  R.SetAsmJSMode(1);
  Block *Entry = NULL;
  LLVMToRelooperMap LLVMToRelooper;

  // Create relooper blocks with their contents. TODO: We could optimize
  // indirectbr by emitting indexed blocks first, so their indexes
  // match up with the label index.
  for (Function::const_iterator BI = F->begin(), BE = F->end();
       BI != BE; ++BI) {
    InvokeState = 0; // each basic block begins in state 0; the previous may not have cleared it, if e.g. it had a throw in the middle and the rest of it was decapitated
    addBlock(BI, R, LLVMToRelooper);
    if (!Entry) Entry = LLVMToRelooper[BI];
  }
  assert(Entry);

  // Create branchings
  for (Function::const_iterator BI = F->begin(), BE = F->end();
       BI != BE; ++BI) {
    const TerminatorInst *TI = BI->getTerminator();
    switch (TI->getOpcode()) {
      default: {
        report_fatal_error("invalid branch instr " + Twine(TI->getOpcodeName()));
        break;
      }
      case Instruction::Br: {
        const BranchInst* br = cast<BranchInst>(TI);
        if (br->getNumOperands() == 3) {
          BasicBlock *S0 = br->getSuccessor(0);
          BasicBlock *S1 = br->getSuccessor(1);
          std::string P0 = getPhiCode(&*BI, S0);
          std::string P1 = getPhiCode(&*BI, S1);
          LLVMToRelooper[&*BI]->AddBranchTo(LLVMToRelooper[&*S0], getValueAsStr(TI->getOperand(0)).c_str(), P0.size() > 0 ? P0.c_str() : NULL);
          LLVMToRelooper[&*BI]->AddBranchTo(LLVMToRelooper[&*S1], NULL,                                     P1.size() > 0 ? P1.c_str() : NULL);
        } else if (br->getNumOperands() == 1) {
          BasicBlock *S = br->getSuccessor(0);
          std::string P = getPhiCode(&*BI, S);
          LLVMToRelooper[&*BI]->AddBranchTo(LLVMToRelooper[&*S], NULL, P.size() > 0 ? P.c_str() : NULL);
        } else {
          error("Branch with 2 operands?");
        }
        break;
      }
      case Instruction::IndirectBr: {
        const IndirectBrInst* br = cast<IndirectBrInst>(TI);
        unsigned Num = br->getNumDestinations();
        std::set<const BasicBlock*> Seen; // sadly llvm allows the same block to appear multiple times
        bool SetDefault = false; // pick the first and make it the default, llvm gives no reasonable default here
        for (unsigned i = 0; i < Num; i++) {
          const BasicBlock *S = br->getDestination(i);
          if (Seen.find(S) != Seen.end()) continue;
          Seen.insert(S);
          std::string P = getPhiCode(&*BI, S);
          std::string Target;
          if (!SetDefault) {
            SetDefault = true;
          } else {
            Target = "case " + utostr(getBlockAddress(F, S)) + ": ";
          }
          LLVMToRelooper[&*BI]->AddBranchTo(LLVMToRelooper[&*S], Target.size() > 0 ? Target.c_str() : NULL, P.size() > 0 ? P.c_str() : NULL);
        }
        break;
      }
      case Instruction::Switch: {
        const SwitchInst* SI = cast<SwitchInst>(TI);
        bool UseSwitch = !!considerConditionVar(SI);
        BasicBlock *DD = SI->getDefaultDest();
        std::string P = getPhiCode(&*BI, DD);
        LLVMToRelooper[&*BI]->AddBranchTo(LLVMToRelooper[&*DD], NULL, P.size() > 0 ? P.c_str() : NULL);
        typedef std::map<const BasicBlock*, std::string> BlockCondMap;
        BlockCondMap BlocksToConditions;
        for (SwitchInst::ConstCaseIt i = SI->case_begin(), e = SI->case_end(); i != e; ++i) {
          const BasicBlock *BB = i.getCaseSuccessor();
          std::string Curr = i.getCaseValue()->getValue().toString(10, true);
          std::string Condition;
          if (UseSwitch) {
            Condition = "case " + Curr + ": ";
          } else {
            Condition = "(" + getValueAsCastParenStr(SI->getCondition()) + " == " + Curr + ")";
          }
          BlocksToConditions[BB] = Condition + (!UseSwitch && BlocksToConditions[BB].size() > 0 ? " | " : "") + BlocksToConditions[BB];
        }
        for (BlockCondMap::const_iterator I = BlocksToConditions.begin(), E = BlocksToConditions.end(); I != E; ++I) {
          const BasicBlock *BB = I->first;
          if (BB == DD) continue; // ok to eliminate this, default dest will get there anyhow
          std::string P = getPhiCode(&*BI, BB);
          LLVMToRelooper[&*BI]->AddBranchTo(LLVMToRelooper[&*BB], I->second.c_str(), P.size() > 0 ? P.c_str() : NULL);
        }
        break;
      }
      case Instruction::Ret:
      case Instruction::Unreachable: break;
    }
  }

  // Calculate relooping and print
  R.Calculate(Entry);
  R.Render();

  // Emit local variables
  UsedVars["sp"] = Type::getInt32Ty(F->getContext());
  unsigned MaxAlignment = Allocas.getMaxAlignment();
  if (MaxAlignment > STACK_ALIGN) {
    UsedVars["sp_a"] = Type::getInt32Ty(F->getContext());
  }
  UsedVars["label"] = Type::getInt32Ty(F->getContext());
  if (!UsedVars.empty()) {
    unsigned Count = 0;
    for (VarMap::const_iterator VI = UsedVars.begin(); VI != UsedVars.end(); ++VI) {
      if (Count == 20) {
        Out << ";\n";
        Count = 0;
      }
      if (Count == 0) Out << " var ";
      if (Count > 0) {
        Out << ", ";
      }
      Count++;
      Out << VI->first << " = ";
      switch (VI->second->getTypeID()) {
        default:
          llvm_unreachable("unsupported variable initializer type");
        case Type::PointerTyID:
        case Type::IntegerTyID:
          Out << "0";
          break;
        case Type::FloatTyID:
          if (PreciseF32) {
            Out << "Math_fround(0)";
            break;
          }
          // otherwise fall through to double
        case Type::DoubleTyID:
          Out << "+0";
          break;
        case Type::VectorTyID:
          if (cast<VectorType>(VI->second)->getElementType()->isIntegerTy()) {
              Out << "SIMD_int32x4(0,0,0,0)";
          } else {
              Out << "SIMD_float32x4(0,0,0,0)";
          }
          break;
      }
    }
    Out << ";";
    nl(Out);
  }

  {
    static bool Warned = false;
    if (!Warned && OptLevel < 2 && UsedVars.size() > 2000) {
      prettyWarning() << "emitted code will contain very large numbers of local variables, which is bad for performance (build to JS with -O2 or above to avoid this - make sure to do so both on source files, and during 'linking')\n";
      Warned = true;
    }
  }

  // Emit stack entry
  Out << " " << getAdHocAssign("sp", Type::getInt32Ty(F->getContext())) << "STACKTOP;";
  if (uint64_t FrameSize = Allocas.getFrameSize()) {
    if (MaxAlignment > STACK_ALIGN) {
      // We must align this entire stack frame to something higher than the default
      Out << "\n ";
      Out << "sp_a = STACKTOP = (STACKTOP + " << utostr(MaxAlignment-1) << ")&-" << utostr(MaxAlignment) << ";";
    }
    Out << "\n ";
    Out << getStackBump(FrameSize);
  }

  // Emit (relooped) code
  char *buffer = Relooper::GetOutputBuffer();
  nl(Out) << buffer;

  // Ensure a final return if necessary
  Type *RT = F->getFunctionType()->getReturnType();
  if (!RT->isVoidTy()) {
    char *LastCurly = strrchr(buffer, '}');
    if (!LastCurly) LastCurly = buffer;
    char *FinalReturn = strstr(LastCurly, "return ");
    if (!FinalReturn) {
      Out << " return " << getParenCast(getConstant(UndefValue::get(RT)), RT, ASM_NONSPECIFIC) << ";\n";
    }
  }
}

void JSWriter::processConstants() {
  // First, calculate the address of each constant
  for (Module::const_global_iterator I = TheModule->global_begin(),
         E = TheModule->global_end(); I != E; ++I) {
    if (I->hasInitializer()) {
      parseConstant(I->getName().str(), I->getInitializer(), true);
    }
  }
  // Second, allocate their contents
  for (Module::const_global_iterator I = TheModule->global_begin(),
         E = TheModule->global_end(); I != E; ++I) {
    if (I->hasInitializer()) {
      parseConstant(I->getName().str(), I->getInitializer(), false);
    }
  }
}

void JSWriter::printFunction(const Function *F) {
  ValueNames.clear();

  // Prepare and analyze function

  UsedVars.clear();
  UniqueNum = 0;

  // When optimizing, the regular optimizer (mem2reg, SROA, GVN, and others)
  // will have already taken all the opportunities for nativization.
  if (OptLevel == CodeGenOpt::None)
    calculateNativizedVars(F);

  // Do alloca coloring at -O1 and higher.
  Allocas.analyze(*F, *DL, OptLevel != CodeGenOpt::None);

  // Emit the function

  std::string Name = F->getName();
  sanitizeGlobal(Name);
  Out << "function " << Name << "(";
  for (Function::const_arg_iterator AI = F->arg_begin(), AE = F->arg_end();
       AI != AE; ++AI) {
    if (AI != F->arg_begin()) Out << ",";
    Out << getJSName(AI);
  }
  Out << ") {";
  nl(Out);
  for (Function::const_arg_iterator AI = F->arg_begin(), AE = F->arg_end();
       AI != AE; ++AI) {
    std::string name = getJSName(AI);
    Out << " " << name << " = " << getCast(name, AI->getType(), ASM_NONSPECIFIC) << ";";
    nl(Out);
  }
  printFunctionBody(F);
  Out << "}";
  nl(Out);

  Allocas.clear();
  StackBumped = false;
}

void JSWriter::printModuleBody() {
  processConstants();

  // Emit function bodies.
  nl(Out) << "// EMSCRIPTEN_START_FUNCTIONS"; nl(Out);
  for (Module::const_iterator I = TheModule->begin(), E = TheModule->end();
       I != E; ++I) {
    if (!I->isDeclaration()) printFunction(I);
  }
  Out << "function runPostSets() {\n";
  Out << " " << PostSets << "\n";
  Out << "}\n";
  PostSets = "";
  Out << "// EMSCRIPTEN_END_FUNCTIONS\n\n";

  assert(GlobalData32.size() == 0 && GlobalData8.size() == 0); // FIXME when we use optimal constant alignments

  // TODO fix commas
  Out << "/* memory initializer */ allocate([";
  printCommaSeparated(GlobalData64);
  if (GlobalData64.size() > 0 && GlobalData32.size() + GlobalData8.size() > 0) {
    Out << ",";
  }
  printCommaSeparated(GlobalData32);
  if (GlobalData32.size() > 0 && GlobalData8.size() > 0) {
    Out << ",";
  }
  printCommaSeparated(GlobalData8);
  Out << "], \"i8\", ALLOC_NONE, Runtime.GLOBAL_BASE);";

  // Emit metadata for emcc driver
  Out << "\n\n// EMSCRIPTEN_METADATA\n";
  Out << "{\n";

  Out << "\"declares\": [";
  bool first = true;
  for (Module::const_iterator I = TheModule->begin(), E = TheModule->end();
       I != E; ++I) {
    if (I->isDeclaration() && !I->use_empty()) {
      // Ignore intrinsics that are always no-ops or expanded into other code
      // which doesn't require the intrinsic function itself to be declared.
      if (I->isIntrinsic()) {
        switch (I->getIntrinsicID()) {
        case Intrinsic::dbg_declare:
        case Intrinsic::dbg_value:
        case Intrinsic::lifetime_start:
        case Intrinsic::lifetime_end:
        case Intrinsic::invariant_start:
        case Intrinsic::invariant_end:
        case Intrinsic::prefetch:
        case Intrinsic::memcpy:
        case Intrinsic::memset:
        case Intrinsic::memmove:
        case Intrinsic::expect:
        case Intrinsic::flt_rounds:
          continue;
        }
      }

      if (first) {
        first = false;
      } else {
        Out << ", ";
      }
      Out << "\"" << I->getName() << "\"";
    }
  }
  for (NameSet::const_iterator I = Declares.begin(), E = Declares.end();
       I != E; ++I) {
    if (first) {
      first = false;
    } else {
      Out << ", ";
    }
    Out << "\"" << *I << "\"";
  }
  Out << "],";

  Out << "\"redirects\": {";
  first = true;
  for (StringMap::const_iterator I = Redirects.begin(), E = Redirects.end();
       I != E; ++I) {
    if (first) {
      first = false;
    } else {
      Out << ", ";
    }
    Out << "\"_" << I->first << "\": \"" << I->second << "\"";
  }
  Out << "},";

  Out << "\"externs\": [";
  first = true;
  for (NameSet::const_iterator I = Externals.begin(), E = Externals.end();
       I != E; ++I) {
    if (first) {
      first = false;
    } else {
      Out << ", ";
    }
    Out << "\"" << *I << "\"";
  }
  Out << "],";

  Out << "\"implementedFunctions\": [";
  first = true;
  for (Module::const_iterator I = TheModule->begin(), E = TheModule->end();
       I != E; ++I) {
    if (!I->isDeclaration()) {
      if (first) {
        first = false;
      } else {
        Out << ", ";
      }
      std::string name = I->getName();
      sanitizeGlobal(name);
      Out << "\"" << name << '"';
    }
  }
  Out << "],";

  Out << "\"tables\": {";
  unsigned Num = FunctionTables.size();
  for (FunctionTableMap::iterator I = FunctionTables.begin(), E = FunctionTables.end(); I != E; ++I) {
    Out << "  \"" << I->first << "\": \"var FUNCTION_TABLE_" << I->first << " = [";
    FunctionTable &Table = I->second;
    // ensure power of two
    unsigned Size = 1;
    while (Size < Table.size()) Size <<= 1;
    while (Table.size() < Size) Table.push_back("0");
    for (unsigned i = 0; i < Table.size(); i++) {
      Out << Table[i];
      if (i < Table.size()-1) Out << ",";
    }
    Out << "];\"";
    if (--Num > 0) Out << ",";
    Out << "\n";
  }
  Out << "},";

  Out << "\"initializers\": [";
  first = true;
  for (unsigned i = 0; i < GlobalInitializers.size(); i++) {
    if (first) {
      first = false;
    } else {
      Out << ", ";
    }
    Out << "\"" << GlobalInitializers[i] << "\"";
  }
  Out << "],";

  Out << "\"exports\": [";
  first = true;
  for (unsigned i = 0; i < Exports.size(); i++) {
    if (first) {
      first = false;
    } else {
      Out << ", ";
    }
    Out << "\"" << Exports[i] << "\"";
  }
  Out << "],";

  Out << "\"cantValidate\": \"" << CantValidate << "\",";

  Out << "\"simd\": ";
  Out << (UsesSIMD ? "1" : "0");
  Out << ",";

  Out << "\"namedGlobals\": {";
  first = true;
  for (NameIntMap::const_iterator I = NamedGlobals.begin(), E = NamedGlobals.end(); I != E; ++I) {
    if (first) {
      first = false;
    } else {
      Out << ", ";
    }
    Out << "\"_" << I->first << "\": \"" << utostr(I->second) << "\"";
  }
  Out << "}";

  Out << "\n}\n";
}

void JSWriter::parseConstant(const std::string& name, const Constant* CV, bool calculate) {
  if (isa<GlobalValue>(CV))
    return;
  //errs() << "parsing constant " << name << "\n";
  // TODO: we repeat some work in both calculate and emit phases here
  // FIXME: use the proper optimal alignments
  if (const ConstantDataSequential *CDS =
         dyn_cast<ConstantDataSequential>(CV)) {
    assert(CDS->isString());
    if (calculate) {
      HeapData *GlobalData = allocateAddress(name);
      StringRef Str = CDS->getAsString();
      for (unsigned int i = 0; i < Str.size(); i++) {
        GlobalData->push_back(Str.data()[i]);
      }
    }
  } else if (const ConstantFP *CFP = dyn_cast<ConstantFP>(CV)) {
    APFloat APF = CFP->getValueAPF();
    if (CFP->getType() == Type::getFloatTy(CFP->getContext())) {
      if (calculate) {
        HeapData *GlobalData = allocateAddress(name);
        union flt { float f; unsigned char b[sizeof(float)]; } flt;
        flt.f = APF.convertToFloat();
        for (unsigned i = 0; i < sizeof(float); ++i) {
          GlobalData->push_back(flt.b[i]);
        }
      }
    } else if (CFP->getType() == Type::getDoubleTy(CFP->getContext())) {
      if (calculate) {
        HeapData *GlobalData = allocateAddress(name);
        union dbl { double d; unsigned char b[sizeof(double)]; } dbl;
        dbl.d = APF.convertToDouble();
        for (unsigned i = 0; i < sizeof(double); ++i) {
          GlobalData->push_back(dbl.b[i]);
        }
      }
    } else {
      assert(false && "Unsupported floating-point type");
    }
  } else if (const ConstantInt *CI = dyn_cast<ConstantInt>(CV)) {
    if (calculate) {
      union { uint64_t i; unsigned char b[sizeof(uint64_t)]; } integer;
      integer.i = *CI->getValue().getRawData();
      unsigned BitWidth = 64; // CI->getValue().getBitWidth();
      assert(BitWidth == 32 || BitWidth == 64);
      HeapData *GlobalData = allocateAddress(name);
      // assuming compiler is little endian
      for (unsigned i = 0; i < BitWidth / 8; ++i) {
        GlobalData->push_back(integer.b[i]);
      }
    }
  } else if (isa<ConstantPointerNull>(CV)) {
    assert(false && "Unlowered ConstantPointerNull");
  } else if (isa<ConstantAggregateZero>(CV)) {
    if (calculate) {
      unsigned Bytes = DL->getTypeStoreSize(CV->getType());
      HeapData *GlobalData = allocateAddress(name);
      for (unsigned i = 0; i < Bytes; ++i) {
        GlobalData->push_back(0);
      }
      // FIXME: create a zero section at the end, avoid filling meminit with zeros
    }
  } else if (const ConstantArray *CA = dyn_cast<ConstantArray>(CV)) {
    if (calculate) {
      for (Constant::const_user_iterator UI = CV->user_begin(), UE = CV->user_end(); UI != UE; ++UI) {
        if ((*UI)->getName() == "llvm.used") {
          // export the kept-alives
          for (unsigned i = 0; i < CA->getNumOperands(); i++) {
            const Constant *C = CA->getOperand(i);
            if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
              C = CE->getOperand(0); // ignore bitcasts
            }
            Exports.push_back(getJSName(C));
          }
        } else if ((*UI)->getName() == "llvm.global.annotations") {
          // llvm.global.annotations can be ignored.
        } else {
          llvm_unreachable("Unexpected constant array");
        }
        break; // we assume one use here
      }
    }
  } else if (const ConstantStruct *CS = dyn_cast<ConstantStruct>(CV)) {
    if (name == "__init_array_start") {
      // this is the global static initializer
      if (calculate) {
        unsigned Num = CS->getNumOperands();
        for (unsigned i = 0; i < Num; i++) {
          const Value* C = CS->getOperand(i);
          if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
            C = CE->getOperand(0); // ignore bitcasts
          }
          GlobalInitializers.push_back(getJSName(C));
        }
      }
    } else if (calculate) {
      HeapData *GlobalData = allocateAddress(name);
      unsigned Bytes = DL->getTypeStoreSize(CV->getType());
      for (unsigned i = 0; i < Bytes; ++i) {
        GlobalData->push_back(0);
      }
    } else {
      // Per the PNaCl abi, this must be a packed struct of a very specific type
      // https://chromium.googlesource.com/native_client/pnacl-llvm/+/7287c45c13dc887cebe3db6abfa2f1080186bb97/lib/Transforms/NaCl/FlattenGlobals.cpp
      assert(CS->getType()->isPacked());
      // This is the only constant where we cannot just emit everything during the first phase, 'calculate', as we may refer to other globals
      unsigned Num = CS->getNumOperands();
      unsigned Offset = getRelativeGlobalAddress(name);
      unsigned OffsetStart = Offset;
      unsigned Absolute = getGlobalAddress(name);
      for (unsigned i = 0; i < Num; i++) {
        const Constant* C = CS->getOperand(i);
        if (isa<ConstantAggregateZero>(C)) {
          unsigned Bytes = DL->getTypeStoreSize(C->getType());
          Offset += Bytes; // zeros, so just skip
        } else if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
          const Value *V = CE->getOperand(0);
          unsigned Data = 0;
          if (CE->getOpcode() == Instruction::PtrToInt) {
            Data = getConstAsOffset(V, Absolute + Offset - OffsetStart);
          } else if (CE->getOpcode() == Instruction::Add) {
            V = cast<ConstantExpr>(V)->getOperand(0);
            Data = getConstAsOffset(V, Absolute + Offset - OffsetStart);
            ConstantInt *CI = cast<ConstantInt>(CE->getOperand(1));
            Data += *CI->getValue().getRawData();
          } else {
            CE->dump();
            llvm_unreachable("Unexpected constant expr kind");
          }
          union { unsigned i; unsigned char b[sizeof(unsigned)]; } integer;
          integer.i = Data;
          assert(Offset+4 <= GlobalData64.size());
          for (unsigned i = 0; i < 4; ++i) {
            GlobalData64[Offset++] = integer.b[i];
          }
        } else if (const ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(C)) {
          assert(CDS->isString());
          StringRef Str = CDS->getAsString();
          assert(Offset+Str.size() <= GlobalData64.size());
          for (unsigned int i = 0; i < Str.size(); i++) {
            GlobalData64[Offset++] = Str.data()[i];
          }
        } else {
          C->dump();
          llvm_unreachable("Unexpected constant kind");
        }
      }
    }
  } else if (isa<ConstantVector>(CV)) {
    assert(false && "Unlowered ConstantVector");
  } else if (isa<BlockAddress>(CV)) {
    assert(false && "Unlowered BlockAddress");
  } else if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(CV)) {
    if (name == "__init_array_start") {
      // this is the global static initializer
      if (calculate) {
        const Value *V = CE->getOperand(0);
        GlobalInitializers.push_back(getJSName(V));
        // is the func
      }
    } else if (name == "__fini_array_start") {
      // nothing to do
    } else {
      // a global equal to a ptrtoint of some function, so a 32-bit integer for us
      if (calculate) {
        HeapData *GlobalData = allocateAddress(name);
        for (unsigned i = 0; i < 4; ++i) {
          GlobalData->push_back(0);
        }
      } else {
        unsigned Data = 0;

        // Deconstruct lowered getelementptrs.
        if (CE->getOpcode() == Instruction::Add) {
          Data = cast<ConstantInt>(CE->getOperand(1))->getZExtValue();
          CE = cast<ConstantExpr>(CE->getOperand(0));
        }
        const Value *V = CE;
        if (CE->getOpcode() == Instruction::PtrToInt) {
          V = CE->getOperand(0);
        }

        // Deconstruct getelementptrs.
        int64_t BaseOffset;
        V = GetPointerBaseWithConstantOffset(V, BaseOffset, DL);
        Data += (uint64_t)BaseOffset;

        Data += getConstAsOffset(V, getGlobalAddress(name));
        union { unsigned i; unsigned char b[sizeof(unsigned)]; } integer;
        integer.i = Data;
        unsigned Offset = getRelativeGlobalAddress(name);
        assert(Offset+4 <= GlobalData64.size());
        for (unsigned i = 0; i < 4; ++i) {
          GlobalData64[Offset++] = integer.b[i];
        }
      }
    }
  } else if (isa<UndefValue>(CV)) {
    assert(false && "Unlowered UndefValue");
  } else {
    CV->dump();
    assert(false && "Unsupported constant kind");
  }
}

// nativization

void JSWriter::calculateNativizedVars(const Function *F) {
  NativizedVars.clear();

  for (Function::const_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    for (BasicBlock::const_iterator II = BI->begin(), E = BI->end(); II != E; ++II) {
      const Instruction *I = &*II;
      if (const AllocaInst *AI = dyn_cast<const AllocaInst>(I)) {
        if (AI->getAllocatedType()->isVectorTy()) continue; // we do not nativize vectors, we rely on the LLVM optimizer to avoid load/stores on them
        if (AI->getAllocatedType()->isAggregateType()) continue; // we do not nativize aggregates either
        // this is on the stack. if its address is never used nor escaped, we can nativize it
        bool Fail = false;
        for (Instruction::const_user_iterator UI = I->user_begin(), UE = I->user_end(); UI != UE && !Fail; ++UI) {
          const Instruction *U = dyn_cast<Instruction>(*UI);
          if (!U) { Fail = true; break; } // not an instruction, not cool
          switch (U->getOpcode()) {
            case Instruction::Load: break; // load is cool
            case Instruction::Store: {
              if (U->getOperand(0) == I) Fail = true; // store *of* it is not cool; store *to* it is fine
              break;
            }
            default: { Fail = true; break; } // anything that is "not" "cool", is "not cool"
          }
        }
        if (!Fail) NativizedVars.insert(I);
      }
    }
  }
}

// special analyses

bool JSWriter::canReloop(const Function *F) {
  return true;
}

// main entry

void JSWriter::printCommaSeparated(const HeapData data) {
  for (HeapData::const_iterator I = data.begin();
       I != data.end(); ++I) {
    if (I != data.begin()) {
      Out << ",";
    }
    Out << (int)*I;
  }
}

void JSWriter::printProgram(const std::string& fname,
                             const std::string& mName) {
  printModule(fname,mName);
}

void JSWriter::printModule(const std::string& fname,
                            const std::string& mName) {
  printModuleBody();
}

bool JSWriter::runOnModule(Module &M) {
  if (M.getTargetTriple() != "asmjs-unknown-emscripten") {
    prettyWarning() << "incorrect target triple '" << M.getTargetTriple() << "' (did you use emcc/em++ on all source files and not clang directly?)\n";
  }

  TheModule = &M;
  DL = &getAnalysis<DataLayoutPass>().getDataLayout();

  setupCallHandlers();

  printProgram("", "");

  return false;
}

char JSWriter::ID = 0;

//===----------------------------------------------------------------------===//
//                       External Interface declaration
//===----------------------------------------------------------------------===//

bool JSTargetMachine::addPassesToEmitFile(PassManagerBase &PM,
                                          formatted_raw_ostream &o,
                                          CodeGenFileType FileType,
                                          bool DisableVerify,
                                          AnalysisID StartAfter,
                                          AnalysisID StopAfter) {
  assert(FileType == TargetMachine::CGFT_AssemblyFile);

  PM.add(createExpandInsertExtractElementPass());
  PM.add(createExpandI64Pass());

  CodeGenOpt::Level OptLevel = getOptLevel();

  // When optimizing, there shouldn't be any opportunities for SimplifyAllocas
  // because the regular optimizer should have taken them all (GVN, and possibly
  // also SROA).
  if (OptLevel == CodeGenOpt::None)
    PM.add(createSimplifyAllocasPass());

  PM.add(new JSWriter(o, OptLevel));

  return false;
}
