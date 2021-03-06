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

#ifndef FORTRAN_FIR_BASICBLOCK_H_
#define FORTRAN_FIR_BASICBLOCK_H_

#include "region.h"
#include <iostream>

namespace Fortran::FIR {

class Region;
class Statement;

class BasicBlock final : public llvm::ilist_node<BasicBlock>,
                         public ChildMixin<BasicBlock, Region>,
                         public Value_impl {
public:
  using StatementListType = llvm::iplist<Statement>;
  using iterator = StatementListType::iterator;
  using const_iterator = StatementListType::const_iterator;
  using reverse_iterator = StatementListType::reverse_iterator;
  using const_reverse_iterator = StatementListType::const_reverse_iterator;

  BasicBlock(const BasicBlock &) = delete;
  BasicBlock &operator=(const BasicBlock &) = delete;
  ~BasicBlock();

  // callback to allow general access to contained sublist(s)
  StatementListType &getSublist(Statement *) { return Statements(); }

  void insertBefore(Statement *stmt, Statement *before = nullptr);

  static BasicBlock *Create(
      Region *parentRegion, BasicBlock *insertBefore = nullptr) {
    return new BasicBlock(parentRegion, insertBefore);
  }
  const Statement *terminator() const;
  Statement *terminator() {
    return const_cast<Statement *>(
        const_cast<const BasicBlock *>(this)->terminator());
  }
  void SetRegion(Region *region) { parent = region; }
  Region *GetRegion() const { return parent; }
  void addPred(BasicBlock *bb);
  std::vector<BasicBlock *> &preds() { return preds_; }
  StatementListType &Statements() { return statementList_; }
  BasicBlock *SplitEdge(BasicBlock *toBlock) { return nullptr; }

private:
  StatementListType statementList_;
  std::vector<BasicBlock *> preds_;
  explicit BasicBlock(Region *parentRegion, BasicBlock *insertBefore);
};

inline std::list<BasicBlock *> pred_list(BasicBlock &block) {
  return std::list<BasicBlock *>{block.preds().begin(), block.preds().end()};
}
}

#endif
