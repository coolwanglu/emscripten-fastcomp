//===-- NaCl.h - NaCl Transformations ---------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_NACL_H
#define LLVM_TRANSFORMS_NACL_H

#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"

namespace llvm {

class BasicBlockPass;
class Function;
class FunctionPass;
class FunctionType;
class Instruction;
class ModulePass;
class Use;
class Value;

BasicBlockPass *createConstantInsertExtractElementIndexPass();
BasicBlockPass *createExpandGetElementPtrPass();
BasicBlockPass *createExpandShuffleVectorPass();
BasicBlockPass *createFixVectorLoadStoreAlignmentPass();
BasicBlockPass *createPromoteI1OpsPass();
FunctionPass *createBackendCanonicalizePass();
FunctionPass *createExpandConstantExprPass();
FunctionPass *createExpandStructRegsPass();
FunctionPass *createInsertDivideCheckPass();
FunctionPass *createPromoteIntegersPass();
FunctionPass *createRemoveAsmMemoryPass();
FunctionPass *createResolvePNaClIntrinsicsPass();
ModulePass *createAddPNaClExternalDeclsPass();
ModulePass *createCanonicalizeMemIntrinsicsPass();
ModulePass *createExpandArithWithOverflowPass();
ModulePass *createExpandByValPass();
ModulePass *createExpandCtorsPass();
ModulePass *createExpandIndirectBrPass();
ModulePass *createExpandSmallArgumentsPass();
ModulePass *createExpandTlsConstantExprPass();
ModulePass *createExpandTlsPass();
ModulePass *createExpandVarArgsPass();
ModulePass *createFlattenGlobalsPass();
ModulePass *createGlobalCleanupPass();
ModulePass *createGlobalizeConstantVectorsPass();
ModulePass *createPNaClSjLjEHPass();
ModulePass *createReplacePtrsWithIntsPass();
ModulePass *createResolveAliasesPass();
ModulePass *createRewriteAtomicsPass();
ModulePass *createRewriteLLVMIntrinsicsPass();
ModulePass *createRewritePNaClLibraryCallsPass();
ModulePass *createStripAttributesPass();
ModulePass *createStripMetadataPass();
ModulePass *createStripModuleFlagsPass();

ModulePass *createExpandI64Pass(); // XXX EMSCRIPTEN
ModulePass *createExpandInsertExtractElementPass(); // XXX EMSCRIPTEN
ModulePass *createLowerEmExceptionsPass(); // XXX EMSCRIPTEN
ModulePass *createLowerEmSetjmpPass(); // XXX EMSCRIPTEN
ModulePass *createNoExitRuntimePass(); // XXX EMSCRIPTEN
ModulePass *createLowerEmAsyncifyPass(); // XXX EMSCRIPTEN

void PNaClABISimplifyAddPreOptPasses(PassManagerBase &PM);
void PNaClABISimplifyAddPostOptPasses(PassManagerBase &PM);

Instruction *PhiSafeInsertPt(Use *U);
void PhiSafeReplaceUses(Use *U, Value *NewVal);

// Copy debug information from Original to New, and return New.
template <typename T> T *CopyDebug(T *New, Instruction *Original) {
  New->setDebugLoc(Original->getDebugLoc());
  return New;
}

template <class InstType>
static void CopyLoadOrStoreAttrs(InstType *Dest, InstType *Src) {
  Dest->setVolatile(Src->isVolatile());
  Dest->setAlignment(Src->getAlignment());
  Dest->setOrdering(Src->getOrdering());
  Dest->setSynchScope(Src->getSynchScope());
}

// In order to change a function's type, the function must be
// recreated.  RecreateFunction() recreates Func with type NewType.
// It copies or moves across everything except the argument values,
// which the caller must update because the argument types might be
// different.
Function *RecreateFunction(Function *Func, FunctionType *NewType);

}

#endif
