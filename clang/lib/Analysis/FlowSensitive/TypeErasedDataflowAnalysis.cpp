//===- TypeErasedDataflowAnalysis.cpp -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines type-erased base types and functions for building dataflow
//  analyses that run over Control-Flow Graphs (CFGs).
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#include "clang/AST/DeclCXX.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/FlowSensitive/DataflowEnvironment.h"
#include "clang/Analysis/FlowSensitive/DataflowWorklist.h"
#include "clang/Analysis/FlowSensitive/Transfer.h"
#include "clang/Analysis/FlowSensitive/TypeErasedDataflowAnalysis.h"
#include "clang/Analysis/FlowSensitive/Value.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/Error.h"

namespace clang {
namespace dataflow {

class StmtToEnvMapImpl : public StmtToEnvMap {
public:
  StmtToEnvMapImpl(
      const ControlFlowContext &CFCtx,
      llvm::ArrayRef<llvm::Optional<TypeErasedDataflowAnalysisState>>
          BlockToState)
      : CFCtx(CFCtx), BlockToState(BlockToState) {}

  const Environment *getEnvironment(const Stmt &S) const override {
    auto BlockIT = CFCtx.getStmtToBlock().find(&S);
    assert(BlockIT != CFCtx.getStmtToBlock().end());
    const auto &State = BlockToState[BlockIT->getSecond()->getBlockID()];
    assert(State.hasValue());
    return &State.getValue().Env;
  }

private:
  const ControlFlowContext &CFCtx;
  llvm::ArrayRef<llvm::Optional<TypeErasedDataflowAnalysisState>> BlockToState;
};

/// Computes the input state for a given basic block by joining the output
/// states of its predecessors.
///
/// Requirements:
///
///   All predecessors of `Block` except those with loop back edges must have
///   already been transferred. States in `BlockStates` that are set to
///   `llvm::None` represent basic blocks that are not evaluated yet.
static TypeErasedDataflowAnalysisState computeBlockInputState(
    const ControlFlowContext &CFCtx,
    llvm::ArrayRef<llvm::Optional<TypeErasedDataflowAnalysisState>> BlockStates,
    const CFGBlock &Block, const Environment &InitEnv,
    TypeErasedDataflowAnalysis &Analysis) {
  llvm::DenseSet<const CFGBlock *> Preds;
  Preds.insert(Block.pred_begin(), Block.pred_end());
  if (Block.getTerminator().isTemporaryDtorsBranch()) {
    // This handles a special case where the code that produced the CFG includes
    // a conditional operator with a branch that constructs a temporary and
    // calls a destructor annotated as noreturn. The CFG models this as follows:
    //
    // B1 (contains the condition of the conditional operator) - succs: B2, B3
    // B2 (contains code that does not call a noreturn destructor) - succs: B4
    // B3 (contains code that calls a noreturn destructor) - succs: B4
    // B4 (has temporary destructor terminator) - succs: B5, B6
    // B5 (noreturn block that is associated with the noreturn destructor call)
    // B6 (contains code that follows the conditional operator statement)
    //
    // The first successor (B5 above) of a basic block with a temporary
    // destructor terminator (B4 above) is the block that evaluates the
    // destructor. If that block has a noreturn element then the predecessor
    // block that constructed the temporary object (B3 above) is effectively a
    // noreturn block and its state should not be used as input for the state
    // of the block that has a temporary destructor terminator (B4 above). This
    // holds regardless of which branch of the ternary operator calls the
    // noreturn destructor. However, it doesn't cases where a nested ternary
    // operator includes a branch that contains a noreturn destructor call.
    //
    // See `NoreturnDestructorTest` for concrete examples.
    if (Block.succ_begin()->getReachableBlock()->hasNoReturnElement()) {
      auto StmtBlock = CFCtx.getStmtToBlock().find(Block.getTerminatorStmt());
      assert(StmtBlock != CFCtx.getStmtToBlock().end());
      Preds.erase(StmtBlock->getSecond());
    }
  }

  llvm::Optional<TypeErasedDataflowAnalysisState> MaybeState;
  for (const CFGBlock *Pred : Preds) {
    // Skip if the `Block` is unreachable or control flow cannot get past it.
    if (!Pred || Pred->hasNoReturnElement())
      continue;

    // Skip if `Pred` was not evaluated yet. This could happen if `Pred` has a
    // loop back edge to `Block`.
    const llvm::Optional<TypeErasedDataflowAnalysisState> &MaybePredState =
        BlockStates[Pred->getBlockID()];
    if (!MaybePredState.hasValue())
      continue;

    const TypeErasedDataflowAnalysisState &PredState =
        MaybePredState.getValue();
    if (MaybeState.hasValue()) {
      Analysis.joinTypeErased(MaybeState->Lattice, PredState.Lattice);
      MaybeState->Env.join(PredState.Env, Analysis);
    } else {
      MaybeState = PredState;
    }
  }
  if (!MaybeState.hasValue()) {
    // FIXME: Consider passing `Block` to `Analysis.typeErasedInitialElement()`
    // to enable building analyses like computation of dominators that
    // initialize the state of each basic block differently.
    MaybeState.emplace(Analysis.typeErasedInitialElement(), InitEnv);
  }
  return *MaybeState;
}

/// Transfers `State` by evaluating `CfgStmt` in the context of `Analysis`.
/// `HandleTransferredStmt` (if provided) will be applied to `CfgStmt`, after it
/// is evaluated.
static void transferCFGStmt(
    const ControlFlowContext &CFCtx,
    llvm::ArrayRef<llvm::Optional<TypeErasedDataflowAnalysisState>> BlockStates,
    const CFGStmt &CfgStmt, TypeErasedDataflowAnalysis &Analysis,
    TypeErasedDataflowAnalysisState &State,
    std::function<void(const CFGStmt &,
                       const TypeErasedDataflowAnalysisState &)>
        HandleTransferredStmt) {
  const Stmt *S = CfgStmt.getStmt();
  assert(S != nullptr);

  if (Analysis.applyBuiltinTransfer())
    transfer(StmtToEnvMapImpl(CFCtx, BlockStates), *S, State.Env);
  Analysis.transferTypeErased(S, State.Lattice, State.Env);

  if (HandleTransferredStmt != nullptr)
    HandleTransferredStmt(CfgStmt, State);
}

/// Transfers `State` by evaluating `CfgInit`.
static void transferCFGInitializer(const CFGInitializer &CfgInit,
                                   TypeErasedDataflowAnalysisState &State) {
  const auto &ThisLoc = *cast<AggregateStorageLocation>(
      State.Env.getThisPointeeStorageLocation());

  const CXXCtorInitializer *Initializer = CfgInit.getInitializer();
  assert(Initializer != nullptr);

  auto *InitStmt = Initializer->getInit();
  assert(InitStmt != nullptr);

  auto *InitStmtLoc =
      State.Env.getStorageLocation(*InitStmt, SkipPast::Reference);
  if (InitStmtLoc == nullptr)
    return;

  auto *InitStmtVal = State.Env.getValue(*InitStmtLoc);
  if (InitStmtVal == nullptr)
    return;

  const FieldDecl *Member = Initializer->getMember();
  assert(Member != nullptr);

  if (Member->getType()->isReferenceType()) {
    auto &MemberLoc = ThisLoc.getChild(*Member);
    State.Env.setValue(MemberLoc,
                       State.Env.takeOwnership(
                           std::make_unique<ReferenceValue>(*InitStmtLoc)));
  } else {
    auto &MemberLoc = ThisLoc.getChild(*Member);
    State.Env.setValue(MemberLoc, *InitStmtVal);
  }
}

TypeErasedDataflowAnalysisState transferBlock(
    const ControlFlowContext &CFCtx,
    std::vector<llvm::Optional<TypeErasedDataflowAnalysisState>> &BlockStates,
    const CFGBlock &Block, const Environment &InitEnv,
    TypeErasedDataflowAnalysis &Analysis,
    std::function<void(const CFGStmt &,
                       const TypeErasedDataflowAnalysisState &)>
        HandleTransferredStmt) {
  TypeErasedDataflowAnalysisState State =
      computeBlockInputState(CFCtx, BlockStates, Block, InitEnv, Analysis);
  for (const CFGElement &Element : Block) {
    switch (Element.getKind()) {
    case CFGElement::Statement:
      transferCFGStmt(CFCtx, BlockStates, *Element.getAs<CFGStmt>(), Analysis,
                      State, HandleTransferredStmt);
      break;
    case CFGElement::Initializer:
      if (Analysis.applyBuiltinTransfer())
        transferCFGInitializer(*Element.getAs<CFGInitializer>(), State);
      break;
    default:
      // FIXME: Evaluate other kinds of `CFGElement`.
      break;
    }
  }
  return State;
}

llvm::Expected<std::vector<llvm::Optional<TypeErasedDataflowAnalysisState>>>
runTypeErasedDataflowAnalysis(const ControlFlowContext &CFCtx,
                              TypeErasedDataflowAnalysis &Analysis,
                              const Environment &InitEnv) {
  PostOrderCFGView POV(&CFCtx.getCFG());
  ForwardDataflowWorklist Worklist(CFCtx.getCFG(), &POV);

  std::vector<llvm::Optional<TypeErasedDataflowAnalysisState>> BlockStates;
  BlockStates.resize(CFCtx.getCFG().size(), llvm::None);

  // The entry basic block doesn't contain statements so it can be skipped.
  const CFGBlock &Entry = CFCtx.getCFG().getEntry();
  BlockStates[Entry.getBlockID()] = {Analysis.typeErasedInitialElement(),
                                     InitEnv};
  Worklist.enqueueSuccessors(&Entry);

  // Bugs in lattices and transfer functions can prevent the analysis from
  // converging. To limit the damage (infinite loops) that these bugs can cause,
  // limit the number of iterations.
  // FIXME: Consider making the maximum number of iterations configurable.
  // FIXME: Set up statistics (see llvm/ADT/Statistic.h) to count average number
  // of iterations, number of functions that time out, etc.
  uint32_t Iterations = 0;
  static constexpr uint32_t MaxIterations = 1 << 16;
  while (const CFGBlock *Block = Worklist.dequeue()) {
    if (++Iterations > MaxIterations) {
      return llvm::createStringError(std::errc::timed_out,
                                     "maximum number of iterations reached");
    }

    const llvm::Optional<TypeErasedDataflowAnalysisState> &OldBlockState =
        BlockStates[Block->getBlockID()];
    TypeErasedDataflowAnalysisState NewBlockState =
        transferBlock(CFCtx, BlockStates, *Block, InitEnv, Analysis);

    if (OldBlockState.hasValue() &&
        Analysis.isEqualTypeErased(OldBlockState.getValue().Lattice,
                                   NewBlockState.Lattice) &&
        OldBlockState->Env.equivalentTo(NewBlockState.Env, Analysis)) {
      // The state of `Block` didn't change after transfer so there's no need to
      // revisit its successors.
      continue;
    }

    BlockStates[Block->getBlockID()] = std::move(NewBlockState);

    // Do not add unreachable successor blocks to `Worklist`.
    if (Block->hasNoReturnElement())
      continue;

    Worklist.enqueueSuccessors(Block);
  }
  // FIXME: Consider evaluating unreachable basic blocks (those that have a
  // state set to `llvm::None` at this point) to also analyze dead code.

  return BlockStates;
}

} // namespace dataflow
} // namespace clang
