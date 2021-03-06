// Copyright (c) 2019, NVIDIA CORPORATION.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef FORTRAN_FIR_BUILDER_H_
#define FORTRAN_FIR_BUILDER_H_

#include "statements.h"
#include <initializer_list>

namespace Fortran::FIR {

/// Helper class for building FIR statements
struct FIRBuilder {
  explicit FIRBuilder(BasicBlock &block)
    : cursorRegion_{block.getParent()}, cursorBlock_{&block} {}

  template<typename A> Statement *Insert(A &&s) {
    CHECK(GetInsertionPoint());
    auto *statement{Statement::Create(GetInsertionPoint(), std::forward<A>(s))};
    return statement;
  }

  template<typename A, typename B> QualifiedStmt<A> QualifiedInsert(B &&s) {
    CHECK(GetInsertionPoint());
    auto *statement{Statement::Create(GetInsertionPoint(), std::forward<B>(s))};
    return QualifiedStmtCreate<A, B>(statement);
  }

  template<typename A> Statement *InsertTerminator(A &&s) {
    auto *stmt{Insert(std::forward<A>(s))};
    for (auto *block : s.succ_blocks()) {
      block->addPred(GetInsertionPoint());
    }
    return stmt;
  }

  // manage the insertion point
  void SetInsertionPoint(BasicBlock *bb) {
    cursorBlock_ = bb;
    cursorRegion_ = bb->getParent();
  }

  void ClearInsertionPoint() { cursorBlock_ = nullptr; }

  BasicBlock *GetInsertionPoint() const { return cursorBlock_; }

  // create the various statements
  QualifiedStmt<Addressable_impl> CreateAddr(const Expression &e) {
    return QualifiedInsert<Addressable_impl>(LocateExprStmt::Create(e));
  }
  QualifiedStmt<Addressable_impl> CreateAddr(Expression &&e) {
    return QualifiedInsert<Addressable_impl>(
        LocateExprStmt::Create(std::move(e)));
  }
  QualifiedStmt<AllocateInsn> CreateAlloc(Type type) {
    return QualifiedInsert<AllocateInsn>(AllocateInsn::Create(type));
  }
  Statement *CreateBranch(BasicBlock *block) {
    return InsertTerminator(BranchStmt::Create(block));
  }
  Statement *CreateCall(
      const FunctionType *type, const Value callee, CallArguments &&args) {
    return Insert(CallStmt::Create(type, callee, std::move(args)));
  }
  Statement *CreateConditionalBranch(
      Statement *cond, BasicBlock *trueBlock, BasicBlock *falseBlock) {
    return InsertTerminator(BranchStmt::Create(cond, trueBlock, falseBlock));
  }
  Statement *CreateDealloc(QualifiedStmt<AllocateInsn> alloc) {
    return Insert(DeallocateInsn::Create(alloc));
  }
  Statement *CreateExpr(const Expression &e) {
    return Insert(ApplyExprStmt::Create(e));
  }
  Statement *CreateExpr(Expression &&e) {
    return Insert(ApplyExprStmt::Create(std::move(e)));
  }
  ApplyExprStmt *MakeAsExpr(const Expression &e) {
    return GetApplyExpr(CreateExpr(e));
  }
  QualifiedStmt<ApplyExprStmt> QualifiedCreateExpr(const Expression &e) {
    return QualifiedInsert<ApplyExprStmt>(ApplyExprStmt::Create(e));
  }
  QualifiedStmt<ApplyExprStmt> QualifiedCreateExpr(Expression &&e) {
    return QualifiedInsert<ApplyExprStmt>(ApplyExprStmt::Create(std::move(e)));
  }
  Statement *CreateIndirectBr(Variable *v, const std::vector<BasicBlock *> &p) {
    return InsertTerminator(IndirectBranchStmt::Create(v, p));
  }
  Statement *CreateIOCall(InputOutputCallType c, IOCallArguments &&a) {
    return Insert(IORuntimeStmt::Create(c, std::move(a)));
  }
  Statement *CreateLoad(Statement *addr) {
    return Insert(LoadInsn::Create(addr));
  }
  QualifiedStmt<Addressable_impl> CreateLocal(
      Type type, const Expression &expr, int alignment = 0) {
    return QualifiedInsert<Addressable_impl>(
        AllocateLocalInsn::Create(type, expr, alignment));
  }
  Statement *CreateNullify(Statement *s) {
    return Insert(DisassociateInsn::Create(s));
  }
  Statement *CreateReturn(QualifiedStmt<ApplyExprStmt> expr) {
    return InsertTerminator(ReturnStmt::Create(expr));
  }
  Statement *CreateRuntimeCall(
      RuntimeCallType call, RuntimeCallArguments &&arguments) {
    return Insert(RuntimeStmt::Create(call, std::move(arguments)));
  }
  Statement *CreateStore(
      QualifiedStmt<Addressable_impl> addr, Statement *value) {
    return Insert(StoreInsn::Create(addr, value));
  }
  Statement *CreateStore(
      QualifiedStmt<Addressable_impl> addr, BasicBlock *value) {
    return Insert(StoreInsn::Create(addr, value));
  }
  Statement *CreateSwitch(
      Value cond, const SwitchStmt::ValueSuccPairListType &pairs) {
    return InsertTerminator(SwitchStmt::Create(cond, pairs));
  }
  Statement *CreateSwitchCase(
      Value cond, const SwitchCaseStmt::ValueSuccPairListType &pairs) {
    return InsertTerminator(SwitchCaseStmt::Create(cond, pairs));
  }
  Statement *CreateSwitchType(
      Value cond, const SwitchTypeStmt::ValueSuccPairListType &pairs) {
    return InsertTerminator(SwitchTypeStmt::Create(cond, pairs));
  }
  Statement *CreateSwitchRank(
      Value cond, const SwitchRankStmt::ValueSuccPairListType &pairs) {
    return InsertTerminator(SwitchRankStmt::Create(cond, pairs));
  }
  Statement *CreateUnreachable() {
    return InsertTerminator(UnreachableStmt::Create());
  }

  void PushBlock(BasicBlock *block) { blockStack_.push_back(block); }
  BasicBlock *PopBlock() {
    auto *block{blockStack_.back()};
    blockStack_.pop_back();
    return block;
  }
  void dump() const;
  void SetCurrentRegion(Region *region) { cursorRegion_ = region; }
  Region *GetCurrentRegion() const { return cursorRegion_; }

private:
  Region *cursorRegion_;
  BasicBlock *cursorBlock_;
  std::vector<BasicBlock *> blockStack_;
};
}

#endif
