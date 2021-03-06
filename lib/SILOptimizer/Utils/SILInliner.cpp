//===--- SILInliner.cpp - Inlines SIL functions ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-inliner"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SIL/SILDebugScope.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
using namespace swift;

bool SILInliner::canInlineFunction(FullApplySite AI) {
  return AI.getFunction() != &Original;
}

/// \brief Inlines the callee of a given ApplyInst (which must be the value of a
/// FunctionRefInst referencing a function with a known body), into the caller
/// containing the ApplyInst, which must be the same function as provided to the
/// constructor of SILInliner. It only performs one step of inlining: it does
/// not recursively inline functions called by the callee.
///
/// It is the responsibility of the caller of this function to delete
/// the given ApplyInst when inlining is successful.
///
/// \returns true on success or false if it is unable to inline the function
/// (for any reason).
void SILInliner::inlineFunction(FullApplySite AI, ArrayRef<SILValue> Args) {
  assert(canInlineFunction(AI) &&
         "Asked to inline function that is unable to be inlined?!");

  // Setup the callee function.
  CalleeFunction = &Original;

  SILFunction &F = getBuilder().getFunction();
  assert(AI.getFunction() && AI.getFunction() == &F &&
         "Inliner called on apply instruction in wrong function?");
  assert(((CalleeFunction->getRepresentation()
             != SILFunctionTypeRepresentation::ObjCMethod &&
           CalleeFunction->getRepresentation()
             != SILFunctionTypeRepresentation::CFunctionPointer) ||
          IKind == InlineKind::PerformanceInline) &&
         "Cannot inline Objective-C methods or C functions in mandatory "
         "inlining");

  CalleeEntryBB = &*CalleeFunction->begin();

  // Compute the SILLocation which should be used by all the inlined
  // instructions.
  if (IKind == InlineKind::PerformanceInline) {
    Loc = InlinedLocation::getInlinedLocation(AI.getLoc());
  } else {
    assert(IKind == InlineKind::MandatoryInline && "Unknown InlineKind.");
    Loc = MandatoryInlinedLocation::getMandatoryInlinedLocation(AI.getLoc());
  }

  auto AIScope = AI.getDebugScope();
  // FIXME: Turn this into an assertion instead.
  if (!AIScope)
    AIScope = AI.getFunction()->getDebugScope();

  if (IKind == InlineKind::MandatoryInline) {
    // Mandatory inlining: every instruction inherits scope/location
    // from the call site.
    CallSiteScope = AIScope;
  } else {
    // Performance inlining. Construct a proper inline scope pointing
    // back to the call site.
    CallSiteScope = new (F.getModule())
        SILDebugScope(AI.getLoc(), nullptr, AIScope, AIScope->InlinedCallSite);
  }
  assert(CallSiteScope && "call site has no scope");
  assert(CallSiteScope->getParentFunction() == &F);

  // Increment the ref count for the inlined function, so it doesn't
  // get deleted before we can emit abstract debug info for it.
  CalleeFunction->setInlined();

  // If the caller's BB is not the last BB in the calling function, then keep
  // track of the next BB so we always insert new BBs before it; otherwise,
  // we just leave the new BBs at the end as they are by default.
  auto IBI = std::next(SILFunction::iterator(AI.getParent()));
  InsertBeforeBB = IBI != F.end() ? &*IBI : nullptr;

  // Clear argument map and map ApplyInst arguments to the arguments of the
  // callee's entry block.
  ValueMap.clear();
  assert(CalleeEntryBB->args_size() == Args.size() &&
         "Unexpected number of arguments to entry block of function?");
  auto BAI = CalleeEntryBB->args_begin();
  for (auto AI = Args.begin(), AE = Args.end(); AI != AE; ++AI, ++BAI)
    ValueMap.insert(std::make_pair(*BAI, *AI));

  InstructionMap.clear();
  BBMap.clear();
  // Do not allow the entry block to be cloned again
  SILBasicBlock::iterator InsertPoint =
    SILBasicBlock::iterator(AI.getInstruction());
  BBMap.insert(std::make_pair(CalleeEntryBB, AI.getParent()));
  getBuilder().setInsertionPoint(InsertPoint);
  // Recursively visit callee's BB in depth-first preorder, starting with the
  // entry block, cloning all instructions other than terminators.
  visitSILBasicBlock(CalleeEntryBB);

  // If we're inlining into a normal apply and the callee's entry
  // block ends in a return, then we can avoid a split.
  if (auto nonTryAI = dyn_cast<ApplyInst>(AI)) {
    if (auto *RI = dyn_cast<ReturnInst>(CalleeEntryBB->getTerminator())) {
      // Replace all uses of the apply instruction with the operands of the
      // return instruction, appropriately mapped.
      nonTryAI->replaceAllUsesWith(remapValue(RI->getOperand()));
      return;
    }
  }

  // If we're inlining into a try_apply, we already have a return-to BB.
  SILBasicBlock *ReturnToBB;
  if (auto tryAI = dyn_cast<TryApplyInst>(AI)) {
    ReturnToBB = tryAI->getNormalBB();

  // Otherwise, split the caller's basic block to create a return-to BB.
  } else {
    SILBasicBlock *CallerBB = AI.getParent();
    // Split the BB and do NOT create a branch between the old and new
    // BBs; we will create the appropriate terminator manually later.
    ReturnToBB = CallerBB->split(InsertPoint);
    // Place the return-to BB after all the other mapped BBs.
    if (InsertBeforeBB)
      F.getBlocks().splice(SILFunction::iterator(InsertBeforeBB), F.getBlocks(),
                           SILFunction::iterator(ReturnToBB));
    else
      F.getBlocks().splice(F.getBlocks().end(), F.getBlocks(),
                           SILFunction::iterator(ReturnToBB));

    // Create an argument on the return-to BB representing the returned value.
    auto *RetArg = ReturnToBB->createPHIArgument(AI.getInstruction()->getType(),
                                                 ValueOwnershipKind::Owned);
    // Replace all uses of the ApplyInst with the new argument.
    AI.getInstruction()->replaceAllUsesWith(RetArg);
  }

  // Now iterate over the callee BBs and fix up the terminators.
  for (auto BI = BBMap.begin(), BE = BBMap.end(); BI != BE; ++BI) {
    getBuilder().setInsertionPoint(BI->second);

    // Modify return terminators to branch to the return-to BB, rather than
    // trying to clone the ReturnInst.
    if (auto *RI = dyn_cast<ReturnInst>(BI->first->getTerminator())) {
      auto thrownValue = remapValue(RI->getOperand());
      getBuilder().createBranch(Loc.getValue(), ReturnToBB,
                                thrownValue);
      continue;
    }

    // Modify throw terminators to branch to the error-return BB, rather than
    // trying to clone the ThrowInst.
    if (auto *TI = dyn_cast<ThrowInst>(BI->first->getTerminator())) {
      if (auto *A = dyn_cast<ApplyInst>(AI)) {
        (void)A;
        assert(A->isNonThrowing() &&
               "apply of a function with error result must be non-throwing");
        getBuilder().createUnreachable(Loc.getValue());
        continue;
      }
      auto tryAI = cast<TryApplyInst>(AI);
      auto returnedValue = remapValue(TI->getOperand());
      getBuilder().createBranch(Loc.getValue(), tryAI->getErrorBB(),
                                returnedValue);
      continue;
    }

    // Otherwise use normal visitor, which clones the existing instruction
    // but remaps basic blocks and values.
    visit(BI->first->getTerminator());
  }
}

void SILInliner::visitDebugValueInst(DebugValueInst *Inst) {
  // The mandatory inliner drops debug_value instructions when inlining, as if
  // it were a "nodebug" function in C.
  if (IKind == InlineKind::MandatoryInline) return;

  return SILCloner<SILInliner>::visitDebugValueInst(Inst);
}
void SILInliner::visitDebugValueAddrInst(DebugValueAddrInst *Inst) {
  // The mandatory inliner drops debug_value_addr instructions when inlining, as
  // if it were a "nodebug" function in C.
  if (IKind == InlineKind::MandatoryInline) return;

  return SILCloner<SILInliner>::visitDebugValueAddrInst(Inst);
}

const SILDebugScope *
SILInliner::getOrCreateInlineScope(const SILDebugScope *CalleeScope) {
  if (!CalleeScope)
    return CallSiteScope;
  auto it = InlinedScopeCache.find(CalleeScope);
  if (it != InlinedScopeCache.end())
    return it->second;

  auto &M = getBuilder().getFunction().getModule();
  auto InlinedAt =
      getOrCreateInlineScope(CalleeScope->InlinedCallSite);
  auto *InlinedScope = new (M) SILDebugScope(
      CalleeScope->Loc, CalleeScope->Parent.dyn_cast<SILFunction *>(),
      CalleeScope->Parent.dyn_cast<const SILDebugScope *>(), InlinedAt);
  InlinedScopeCache.insert({CalleeScope, InlinedScope});
  return InlinedScope;
}

//===----------------------------------------------------------------------===//
//                                 Cost Model
//===----------------------------------------------------------------------===//

static InlineCost getEnforcementCost(SILAccessEnforcement enforcement) {
  switch (enforcement) {
  case SILAccessEnforcement::Unknown:
    llvm_unreachable("evaluating cost of access with unknown enforcement?");
  case SILAccessEnforcement::Dynamic:
    return InlineCost::Expensive;
  case SILAccessEnforcement::Static:
  case SILAccessEnforcement::Unsafe:
    return InlineCost::Free;
  }
  llvm_unreachable("bad enforcement");
}

/// For now just assume that every SIL instruction is one to one with an LLVM
/// instruction. This is of course very much so not true.
InlineCost swift::instructionInlineCost(SILInstruction &I) {
  switch (I.getKind()) {
    case ValueKind::IntegerLiteralInst:
    case ValueKind::FloatLiteralInst:
    case ValueKind::DebugValueInst:
    case ValueKind::DebugValueAddrInst:
    case ValueKind::StringLiteralInst:
    case ValueKind::ConstStringLiteralInst:
    case ValueKind::FixLifetimeInst:
    case ValueKind::EndBorrowInst:
    case ValueKind::EndBorrowArgumentInst:
    case ValueKind::BeginBorrowInst:
    case ValueKind::MarkDependenceInst:
    case ValueKind::FunctionRefInst:
    case ValueKind::AllocGlobalInst:
    case ValueKind::GlobalAddrInst:
    case ValueKind::EndLifetimeInst:
    case ValueKind::UncheckedOwnershipConversionInst:
      return InlineCost::Free;

    // Typed GEPs are free.
    case ValueKind::TupleElementAddrInst:
    case ValueKind::StructElementAddrInst:
    case ValueKind::ProjectBlockStorageInst:
      return InlineCost::Free;

    // Aggregates are exploded at the IR level; these are effectively no-ops.
    case ValueKind::TupleInst:
    case ValueKind::StructInst:
    case ValueKind::StructExtractInst:
    case ValueKind::TupleExtractInst:
      return InlineCost::Free;

    // Unchecked casts are free.
    case ValueKind::AddressToPointerInst:
    case ValueKind::PointerToAddressInst:

    case ValueKind::UncheckedRefCastInst:
    case ValueKind::UncheckedRefCastAddrInst:
    case ValueKind::UncheckedAddrCastInst:
    case ValueKind::UncheckedTrivialBitCastInst:
    case ValueKind::UncheckedBitwiseCastInst:

    case ValueKind::RawPointerToRefInst:
    case ValueKind::RefToRawPointerInst:

    case ValueKind::UpcastInst:

    case ValueKind::ThinToThickFunctionInst:
    case ValueKind::ThinFunctionToPointerInst:
    case ValueKind::PointerToThinFunctionInst:
    case ValueKind::ConvertFunctionInst:

    case ValueKind::BridgeObjectToWordInst:
      return InlineCost::Free;

    // Access instructions are free unless we're dynamically enforcing them.
    case ValueKind::BeginAccessInst:
      return getEnforcementCost(cast<BeginAccessInst>(I).getEnforcement());
    case ValueKind::EndAccessInst:
      return getEnforcementCost(cast<EndAccessInst>(I).getBeginAccess()
                                                     ->getEnforcement());
    case ValueKind::BeginUnpairedAccessInst:
      return getEnforcementCost(cast<BeginUnpairedAccessInst>(I)
                                  .getEnforcement());
    case ValueKind::EndUnpairedAccessInst:
      return getEnforcementCost(cast<EndUnpairedAccessInst>(I)
                                  .getEnforcement());

    // TODO: These are free if the metatype is for a Swift class.
    case ValueKind::ThickToObjCMetatypeInst:
    case ValueKind::ObjCToThickMetatypeInst:
      return InlineCost::Expensive;
      
    // TODO: Bridge object conversions imply a masking operation that should be
    // "hella cheap" but not really expensive
    case ValueKind::BridgeObjectToRefInst:
    case ValueKind::RefToBridgeObjectInst:
      return InlineCost::Expensive;

    case ValueKind::MetatypeInst:
      // Thin metatypes are always free.
      if (I.getType().castTo<MetatypeType>()->getRepresentation()
            == MetatypeRepresentation::Thin)
        return InlineCost::Free;
      // TODO: Thick metatypes are free if they don't require generic or lazy
      // instantiation.
      return InlineCost::Expensive;

    // Protocol descriptor references are free.
    case ValueKind::ObjCProtocolInst:
      return InlineCost::Free;

    // Metatype-to-object conversions are free.
    case ValueKind::ObjCExistentialMetatypeToObjectInst:
    case ValueKind::ObjCMetatypeToObjectInst:
      return InlineCost::Free;

    // Return and unreachable are free.
    case ValueKind::UnreachableInst:
    case ValueKind::ReturnInst:
    case ValueKind::ThrowInst:
      return InlineCost::Free;

    case ValueKind::ApplyInst:
    case ValueKind::TryApplyInst:
    case ValueKind::AllocBoxInst:
    case ValueKind::AllocExistentialBoxInst:
    case ValueKind::AllocRefInst:
    case ValueKind::AllocRefDynamicInst:
    case ValueKind::AllocStackInst:
    case ValueKind::AllocValueBufferInst:
    case ValueKind::BindMemoryInst:
    case ValueKind::ValueMetatypeInst:
    case ValueKind::WitnessMethodInst:
    case ValueKind::AssignInst:
    case ValueKind::BranchInst:
    case ValueKind::CheckedCastBranchInst:
    case ValueKind::CheckedCastValueBranchInst:
    case ValueKind::CheckedCastAddrBranchInst:
    case ValueKind::ClassMethodInst:
    case ValueKind::CondBranchInst:
    case ValueKind::CondFailInst:
    case ValueKind::CopyBlockInst:
    case ValueKind::CopyAddrInst:
    case ValueKind::RetainValueInst:
    case ValueKind::RetainValueAddrInst:
    case ValueKind::UnmanagedRetainValueInst:
    case ValueKind::CopyValueInst:
    case ValueKind::CopyUnownedValueInst:
    case ValueKind::DeallocBoxInst:
    case ValueKind::DeallocExistentialBoxInst:
    case ValueKind::DeallocRefInst:
    case ValueKind::DeallocPartialRefInst:
    case ValueKind::DeallocStackInst:
    case ValueKind::DeallocValueBufferInst:
    case ValueKind::DeinitExistentialAddrInst:
    case ValueKind::DeinitExistentialValueInst:
    case ValueKind::DestroyAddrInst:
    case ValueKind::ProjectValueBufferInst:
    case ValueKind::ProjectBoxInst:
    case ValueKind::ProjectExistentialBoxInst:
    case ValueKind::ReleaseValueInst:
    case ValueKind::ReleaseValueAddrInst:
    case ValueKind::UnmanagedReleaseValueInst:
    case ValueKind::DestroyValueInst:
    case ValueKind::AutoreleaseValueInst:
    case ValueKind::UnmanagedAutoreleaseValueInst:
    case ValueKind::DynamicMethodBranchInst:
    case ValueKind::DynamicMethodInst:
    case ValueKind::EnumInst:
    case ValueKind::IndexAddrInst:
    case ValueKind::TailAddrInst:
    case ValueKind::IndexRawPointerInst:
    case ValueKind::InitEnumDataAddrInst:
    case ValueKind::InitExistentialAddrInst:
    case ValueKind::InitExistentialValueInst:
    case ValueKind::InitExistentialMetatypeInst:
    case ValueKind::InitExistentialRefInst:
    case ValueKind::InjectEnumAddrInst:
    case ValueKind::IsNonnullInst:
    case ValueKind::LoadInst:
    case ValueKind::LoadBorrowInst:
    case ValueKind::LoadUnownedInst:
    case ValueKind::LoadWeakInst:
    case ValueKind::OpenExistentialAddrInst:
    case ValueKind::OpenExistentialBoxInst:
    case ValueKind::OpenExistentialBoxValueInst:
    case ValueKind::OpenExistentialMetatypeInst:
    case ValueKind::OpenExistentialRefInst:
    case ValueKind::OpenExistentialValueInst:
    case ValueKind::PartialApplyInst:
    case ValueKind::ExistentialMetatypeInst:
    case ValueKind::RefElementAddrInst:
    case ValueKind::RefTailAddrInst:
    case ValueKind::RefToUnmanagedInst:
    case ValueKind::RefToUnownedInst:
    case ValueKind::StoreInst:
    case ValueKind::StoreBorrowInst:
    case ValueKind::StoreUnownedInst:
    case ValueKind::StoreWeakInst:
    case ValueKind::StrongPinInst:
    case ValueKind::StrongReleaseInst:
    case ValueKind::SetDeallocatingInst:
    case ValueKind::StrongRetainInst:
    case ValueKind::StrongRetainUnownedInst:
    case ValueKind::StrongUnpinInst:
    case ValueKind::SuperMethodInst:
    case ValueKind::SwitchEnumAddrInst:
    case ValueKind::SwitchEnumInst:
    case ValueKind::SwitchValueInst:
    case ValueKind::UncheckedEnumDataInst:
    case ValueKind::UncheckedTakeEnumDataAddrInst:
    case ValueKind::UnconditionalCheckedCastInst:
    case ValueKind::UnconditionalCheckedCastAddrInst:
    case ValueKind::UnconditionalCheckedCastValueInst:
    case ValueKind::UnmanagedToRefInst:
    case ValueKind::UnownedReleaseInst:
    case ValueKind::UnownedRetainInst:
    case ValueKind::IsUniqueInst:
    case ValueKind::IsUniqueOrPinnedInst:
    case ValueKind::UnownedToRefInst:
    case ValueKind::InitBlockStorageHeaderInst:
    case ValueKind::SelectEnumAddrInst:
    case ValueKind::SelectEnumInst:
    case ValueKind::SelectValueInst:
    case ValueKind::KeyPathInst:
    case ValueKind::GlobalValueInst:
      return InlineCost::Expensive;

    case ValueKind::BuiltinInst: {
      auto *BI = cast<BuiltinInst>(&I);
      // Expect intrinsics are 'free' instructions.
      if (BI->getIntrinsicInfo().ID == llvm::Intrinsic::expect)
        return InlineCost::Free;
      if (BI->getBuiltinInfo().ID == BuiltinValueKind::OnFastPath)
        return InlineCost::Free;

      return InlineCost::Expensive;
    }
    case ValueKind::SILPHIArgument:
    case ValueKind::SILFunctionArgument:
    case ValueKind::SILUndef:
      llvm_unreachable("Only instructions should be passed into this "
                       "function.");
    case ValueKind::MarkFunctionEscapeInst:
    case ValueKind::MarkUninitializedInst:
    case ValueKind::MarkUninitializedBehaviorInst:
      llvm_unreachable("not valid in canonical sil");
    case ValueKind::ObjectInst:
      llvm_unreachable("not valid in a function");
  }

  llvm_unreachable("Unhandled ValueKind in switch.");
}
