// Copyright (c) 2018-2019, NVIDIA CORPORATION.  All rights reserved.
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

#include "expression.h"
#include "common.h"
#include "int-power.h"
#include "tools.h"
#include "variable.h"
#include "../common/idioms.h"
#include "../parser/message.h"
#include <string>
#include <type_traits>

using namespace Fortran::parser::literals;

namespace Fortran::evaluate {

template<int KIND>
Expr<SubscriptInteger> Expr<Type<TypeCategory::Character, KIND>>::LEN() const {
  return std::visit(
      common::visitors{
          [](const Constant<Result> &c) {
            return AsExpr(Constant<SubscriptInteger>{c.LEN()});
          },
          [](const ArrayConstructor<Result> &a) { return a.LEN(); },
          [](const Parentheses<Result> &x) { return x.left().LEN(); },
          [](const Convert<Result> &x) {
            return std::visit(
                [&](const auto &kx) { return kx.LEN(); }, x.left().u);
          },
          [](const Concat<KIND> &c) {
            return c.left().LEN() + c.right().LEN();
          },
          [](const Extremum<Result> &c) {
            return Expr<SubscriptInteger>{
                Extremum<SubscriptInteger>{c.left().LEN(), c.right().LEN()}};
          },
          [](const Designator<Result> &dr) { return dr.LEN(); },
          [](const FunctionRef<Result> &fr) { return fr.LEN(); },
          [](const SetLength<KIND> &x) { return x.right(); },
      },
      u);
}

Expr<SomeType>::~Expr() = default;

#if defined(__APPLE__) && defined(__GNUC__)
template<typename A>
typename ExpressionBase<A>::Derived &ExpressionBase<A>::derived() {
  return *static_cast<Derived *>(this);
}

template<typename A>
const typename ExpressionBase<A>::Derived &ExpressionBase<A>::derived() const {
  return *static_cast<const Derived *>(this);
}
#endif

template<typename A>
std::optional<DynamicType> ExpressionBase<A>::GetType() const {
  if constexpr (IsLengthlessIntrinsicType<Result>) {
    return Result::GetType();
  } else {
    return std::visit(
        [&](const auto &x) -> std::optional<DynamicType> {
          if constexpr (!common::HasMember<decltype(x), TypelessExpression>) {
            return x.GetType();
          }
          return std::nullopt;
        },
        derived().u);
  }
}

template<typename A> int ExpressionBase<A>::Rank() const {
  return std::visit(
      [](const auto &x) {
        if constexpr (common::HasMember<decltype(x), TypelessExpression>) {
          return 0;
        } else {
          return x.Rank();
        }
      },
      derived().u);
}

// Equality testing for classes without EVALUATE_UNION_CLASS_BOILERPLATE()

bool ImpliedDoIndex::operator==(const ImpliedDoIndex &that) const {
  return name == that.name;
}

template<typename T>
bool ImpliedDo<T>::operator==(const ImpliedDo<T> &that) const {
  return name_ == that.name_ && lower_ == that.lower_ &&
      upper_ == that.upper_ && stride_ == that.stride_ &&
      values_ == that.values_;
}

template<typename R>
bool ArrayConstructorValues<R>::operator==(
    const ArrayConstructorValues<R> &that) const {
  return values_ == that.values_;
}

template<int KIND>
bool ArrayConstructor<Type<TypeCategory::Character, KIND>>::operator==(
    const ArrayConstructor &that) const {
  return length_ == that.length_ &&
      static_cast<const Base &>(*this) == static_cast<const Base &>(that);
}

bool ArrayConstructor<SomeDerived>::operator==(
    const ArrayConstructor &that) const {
  return derivedTypeSpec_ == that.derivedTypeSpec_ &&
      static_cast<const Base &>(*this) == static_cast<const Base &>(that);
  ;
}

StructureConstructor::StructureConstructor(
    const semantics::DerivedTypeSpec &spec,
    const StructureConstructorValues &values)
  : derivedTypeSpec_{&spec}, values_{values} {}
StructureConstructor::StructureConstructor(
    const semantics::DerivedTypeSpec &spec, StructureConstructorValues &&values)
  : derivedTypeSpec_{&spec}, values_{std::move(values)} {}

bool StructureConstructor::operator==(const StructureConstructor &that) const {
  return derivedTypeSpec_ == that.derivedTypeSpec_ && values_ == that.values_;
}

DynamicType StructureConstructor::GetType() const {
  return DynamicType{*derivedTypeSpec_};
}

StructureConstructor &StructureConstructor::Add(
    const Symbol &symbol, Expr<SomeType> &&expr) {
  values_.emplace(&symbol, std::move(expr));
  return *this;
}

GenericExprWrapper::~GenericExprWrapper() = default;

bool GenericExprWrapper::operator==(const GenericExprWrapper &that) const {
  return v == that.v;
}

template<TypeCategory CAT> int Expr<SomeKind<CAT>>::GetKind() const {
  return std::visit(
      [](const auto &kx) { return std::decay_t<decltype(kx)>::Result::kind; },
      u);
}

int Expr<SomeCharacter>::GetKind() const {
  return std::visit(
      [](const auto &kx) { return std::decay_t<decltype(kx)>::Result::kind; },
      u);
}

Expr<SubscriptInteger> Expr<SomeCharacter>::LEN() const {
  return std::visit([](const auto &kx) { return kx.LEN(); }, u);
}

INSTANTIATE_EXPRESSION_TEMPLATES
}
DEFINE_DELETER(Fortran::evaluate::GenericExprWrapper)
