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

#ifndef FORTRAN_EVALUATE_EXPRESSION_H_
#define FORTRAN_EVALUATE_EXPRESSION_H_

// Represent Fortran expressions in a type-safe manner.
// Expressions are the sole owners of their constituents; i.e., there is no
// context-independent hash table or sharing of common subexpressions, and
// thus these are trees, not DAGs.  Both deep copy and move semantics are
// supported for expression construction.  Expressions may be compared
// for equality.

#include "common.h"
#include "constant.h"
#include "formatting.h"
#include "type.h"
#include "variable.h"
#include "../common/Fortran.h"
#include "../common/idioms.h"
#include "../common/indirection.h"
#include "../common/template.h"
#include "../parser/char-block.h"
#include <algorithm>
#include <list>
#include <ostream>
#include <tuple>
#include <type_traits>
#include <variant>

namespace Fortran::evaluate {

using common::RelationalOperator;

// Expressions are represented by specializations of the class template Expr.
// Each of these specializations wraps a single data member "u" that
// is a std::variant<> discriminated union over all of the representational
// types for the constants, variables, operations, and other entities that
// can be valid expressions in that context:
// - Expr<Type<CATEGORY, KIND>> represents an expression whose result is of a
//   specific intrinsic type category and kind, e.g. Type<TypeCategory::Real, 4>
// - Expr<SomeDerived> wraps data and procedure references that result in an
//   instance of a derived type
// - Expr<SomeKind<CATEGORY>> is a union of Expr<Type<CATEGORY, K>> for each
//   kind type parameter value K in that intrinsic type category.  It represents
//   an expression with known category and any kind.
// - Expr<SomeType> is a union of Expr<SomeKind<CATEGORY>> over the five
//   intrinsic type categories of Fortran.  It represents any valid expression.
//
// Everything that can appear in, or as, a valid Fortran expression must be
// represented with an instance of some class containing a Result typedef that
// maps to some instantiation of Type<CATEGORY, KIND>, SomeKind<CATEGORY>,
// or SomeType.  (Exception: BOZ literal constants in generic Expr<SomeType>.)
template<typename A> using ResultType = typename std::decay_t<A>::Result;

// Common Expr<> behaviors: every Expr<T> derives from ExpressionBase<T>.
template<typename RESULT> class ExpressionBase {
public:
  using Result = RESULT;

private:
  using Derived = Expr<Result>;
#if defined(__APPLE__) && defined(__GNUC__)
  Derived &derived();
  const Derived &derived() const;
#else
  Derived &derived() { return *static_cast<Derived *>(this); }
  const Derived &derived() const { return *static_cast<const Derived *>(this); }
#endif

public:
  template<typename A> Derived &operator=(const A &x) {
    Derived &d{derived()};
    d.u = x;
    return d;
  }

  template<typename A> common::IfNoLvalue<Derived &, A> operator=(A &&x) {
    Derived &d{derived()};
    d.u = std::move(x);
    return d;
  }

  std::optional<DynamicType> GetType() const;
  int Rank() const;
  std::ostream &AsFortran(std::ostream &) const;
  static Derived Rewrite(FoldingContext &, Derived &&);
};

// Operations always have specific Fortran result types (i.e., with known
// intrinsic type category and kind parameter value).  The classes that
// represent the operations all inherit from this Operation<> base class
// template.  Note that Operation has as its first type parameter (DERIVED) a
// "curiously reoccurring template pattern (CRTP)" reference to the specific
// operation class being derived from Operation; e.g., Add is defined with
// struct Add : public Operation<Add, ...>.  Uses of instances of Operation<>,
// including its own member functions, can access each specific class derived
// from it via its derived() member function with compile-time type safety.
template<typename DERIVED, typename RESULT, typename... OPERANDS>
class Operation {
  // The extra final member is a dummy that allows a safe unused reference
  // to element 1 to arise indirectly in the definition of "right()" below
  // when the operation has but a single operand.
  using OperandTypes = std::tuple<OPERANDS..., std::monostate>;

public:
  using Derived = DERIVED;
  using Result = RESULT;
  static_assert(IsSpecificIntrinsicType<Result>);
  static constexpr std::size_t operands{sizeof...(OPERANDS)};
  template<int J> using Operand = std::tuple_element_t<J, OperandTypes>;

  // Unary operations wrap a single Expr with a CopyableIndirection.
  // Binary operations wrap a tuple of CopyableIndirections to Exprs.
private:
  using Container = std::conditional_t<operands == 1,
      common::CopyableIndirection<Expr<Operand<0>>>,
      std::tuple<common::CopyableIndirection<Expr<OPERANDS>>...>>;

public:
  CLASS_BOILERPLATE(Operation)
  explicit Operation(const Expr<OPERANDS> &... x) : operand_{x...} {}
  explicit Operation(Expr<OPERANDS> &&... x) : operand_{std::move(x)...} {}

  Derived &derived() { return *static_cast<Derived *>(this); }
  const Derived &derived() const { return *static_cast<const Derived *>(this); }

  // References to operand expressions from member functions of derived
  // classes for specific operators can be made by index, e.g. operand<0>(),
  // which must be spelled like "this->template operand<0>()" when
  // inherited in a derived class template.  There are convenience aliases
  // left() and right() that are not templates.
  template<int J> Expr<Operand<J>> &operand() {
    if constexpr (operands == 1) {
      static_assert(J == 0);
      return operand_.value();
    } else {
      return std::get<J>(operand_).value();
    }
  }
  template<int J> const Expr<Operand<J>> &operand() const {
    if constexpr (operands == 1) {
      static_assert(J == 0);
      return operand_.value();
    } else {
      return std::get<J>(operand_).value();
    }
  }

  Expr<Operand<0>> &left() { return operand<0>(); }
  const Expr<Operand<0>> &left() const { return operand<0>(); }

  std::conditional_t<(operands > 1), Expr<Operand<1>> &, void> right() {
    if constexpr (operands > 1) {
      return operand<1>();
    }
  }
  std::conditional_t<(operands > 1), const Expr<Operand<1>> &, void>
  right() const {
    if constexpr (operands > 1) {
      return operand<1>();
    }
  }

  static constexpr std::optional<DynamicType> GetType() {
    return Result::GetType();
  }
  int Rank() const {
    int rank{left().Rank()};
    if constexpr (operands > 1) {
      return std::max(rank, right().Rank());
    } else {
      return rank;
    }
  }

  bool operator==(const Operation &that) const {
    return operand_ == that.operand_;
  }

  std::ostream &AsFortran(std::ostream &) const;

protected:
  // Overridable functions for AsFortran()
  static const char *Prefix() { return ""; }
  static const char *Infix() { return ""; }
  static const char *Suffix() { return ""; }

private:
  Container operand_;
};

// Unary operations

// Conversions to specific types from expressions of known category and
// dynamic kind.
template<typename TO, TypeCategory FROMCAT = TO::category>
struct Convert : public Operation<Convert<TO, FROMCAT>, TO, SomeKind<FROMCAT>> {
  // Fortran doesn't have conversions between kinds of CHARACTER apart from
  // assignments, and in those the data must be convertible to/from 7-bit ASCII.
  // Conversions between kinds of COMPLEX are represented piecewise.
  static_assert(((TO::category == TypeCategory::Integer ||
                     TO::category == TypeCategory::Real) &&
                    (FROMCAT == TypeCategory::Integer ||
                        FROMCAT == TypeCategory::Real)) ||
      (TO::category == TypeCategory::Character &&
          FROMCAT == TypeCategory::Character) ||
      (TO::category == TypeCategory::Logical &&
          FROMCAT == TypeCategory::Logical));
  using Result = TO;
  using Operand = SomeKind<FROMCAT>;
  using Base = Operation<Convert, Result, Operand>;
  using Base::Base;
  std::ostream &AsFortran(std::ostream &) const;
};

template<typename A>
struct Parentheses : public Operation<Parentheses<A>, A, A> {
  using Result = A;
  using Operand = A;
  using Base = Operation<Parentheses, A, A>;
  using Base::Base;
  static const char *Prefix() { return "("; }
  static const char *Suffix() { return ")"; }
};

template<typename A> struct Negate : public Operation<Negate<A>, A, A> {
  using Result = A;
  using Operand = A;
  using Base = Operation<Negate, A, A>;
  using Base::Base;
  static const char *Prefix() { return "-"; }
};

template<int KIND>
struct ComplexComponent
  : public Operation<ComplexComponent<KIND>, Type<TypeCategory::Real, KIND>,
        Type<TypeCategory::Complex, KIND>> {
  using Result = Type<TypeCategory::Real, KIND>;
  using Operand = Type<TypeCategory::Complex, KIND>;
  using Base = Operation<ComplexComponent, Result, Operand>;
  CLASS_BOILERPLATE(ComplexComponent)
  ComplexComponent(bool isImaginary, const Expr<Operand> &x)
    : Base{x}, isImaginaryPart{isImaginary} {}
  ComplexComponent(bool isImaginary, Expr<Operand> &&x)
    : Base{std::move(x)}, isImaginaryPart{isImaginary} {}

  const char *Suffix() const { return isImaginaryPart ? "%IM)" : "%RE)"; }

  bool isImaginaryPart{true};
};

template<int KIND>
struct Not : public Operation<Not<KIND>, Type<TypeCategory::Logical, KIND>,
                 Type<TypeCategory::Logical, KIND>> {
  using Result = Type<TypeCategory::Logical, KIND>;
  using Operand = Result;
  using Base = Operation<Not, Result, Operand>;
  using Base::Base;
  static const char *Prefix() { return ".NOT."; }
};

// Character lengths are determined by context in Fortran and do not
// have explicit syntax for changing them.  Expressions represent
// changes of length (e.g., for assignments and structure constructors)
// with this operation.
template<int KIND>
struct SetLength
  : public Operation<SetLength<KIND>, Type<TypeCategory::Character, KIND>,
        Type<TypeCategory::Character, KIND>, SubscriptInteger> {
  using Result = Type<TypeCategory::Character, KIND>;
  using CharacterOperand = Result;
  using LengthOperand = SubscriptInteger;
  using Base = Operation<SetLength, Result, CharacterOperand, LengthOperand>;
  using Base::Base;
  static const char *Prefix() { return "%SET_LENGTH("; }
};

// Binary operations

template<typename A> struct Add : public Operation<Add<A>, A, A, A> {
  using Result = A;
  using Operand = A;
  using Base = Operation<Add, A, A, A>;
  using Base::Base;
  static const char *Infix() { return "+"; }
};

template<typename A> struct Subtract : public Operation<Subtract<A>, A, A, A> {
  using Result = A;
  using Operand = A;
  using Base = Operation<Subtract, A, A, A>;
  using Base::Base;
  static const char *Infix() { return "-"; }
};

template<typename A> struct Multiply : public Operation<Multiply<A>, A, A, A> {
  using Result = A;
  using Operand = A;
  using Base = Operation<Multiply, A, A, A>;
  using Base::Base;
  static const char *Infix() { return "*"; }
};

template<typename A> struct Divide : public Operation<Divide<A>, A, A, A> {
  using Result = A;
  using Operand = A;
  using Base = Operation<Divide, A, A, A>;
  using Base::Base;
  static const char *Infix() { return "/"; }
};

template<typename A> struct Power : public Operation<Power<A>, A, A, A> {
  using Result = A;
  using Operand = A;
  using Base = Operation<Power, A, A, A>;
  using Base::Base;
  static const char *Infix() { return "**"; }
};

template<typename A>
struct RealToIntPower : public Operation<RealToIntPower<A>, A, A, SomeInteger> {
  using Base = Operation<RealToIntPower, A, A, SomeInteger>;
  using Result = A;
  using BaseOperand = A;
  using ExponentOperand = SomeInteger;
  using Base::Base;
  static const char *Infix() { return "**"; }
};

template<typename A> struct Extremum : public Operation<Extremum<A>, A, A, A> {
  using Result = A;
  using Operand = A;
  using Base = Operation<Extremum, A, A, A>;
  CLASS_BOILERPLATE(Extremum)
  Extremum(const Expr<Operand> &x, const Expr<Operand> &y,
      Ordering ord = Ordering::Greater)
    : Base{x, y}, ordering{ord} {}
  Extremum(
      Expr<Operand> &&x, Expr<Operand> &&y, Ordering ord = Ordering::Greater)
    : Base{std::move(x), std::move(y)}, ordering{ord} {}

  const char *Prefix() const {
    return ordering == Ordering::Less ? "MIN(" : "MAX(";
  }

  Ordering ordering{Ordering::Greater};
};

template<int KIND>
struct ComplexConstructor
  : public Operation<ComplexConstructor<KIND>,
        Type<TypeCategory::Complex, KIND>, Type<TypeCategory::Real, KIND>,
        Type<TypeCategory::Real, KIND>> {
  using Result = Type<TypeCategory::Complex, KIND>;
  using Operand = Type<TypeCategory::Real, KIND>;
  using Base = Operation<ComplexConstructor, Result, Operand, Operand>;
  using Base::Base;
  static const char *Prefix() { return "("; }
  static const char *Infix() { return ","; }
  static const char *Suffix() { return ")"; }
};

template<int KIND>
struct Concat
  : public Operation<Concat<KIND>, Type<TypeCategory::Character, KIND>,
        Type<TypeCategory::Character, KIND>,
        Type<TypeCategory::Character, KIND>> {
  using Result = Type<TypeCategory::Character, KIND>;
  using Operand = Result;
  using Base = Operation<Concat, Result, Operand, Operand>;
  using Base::Base;
  static const char *Infix() { return "//"; }
};

ENUM_CLASS(LogicalOperator, And, Or, Eqv, Neqv)

template<int KIND>
struct LogicalOperation
  : public Operation<LogicalOperation<KIND>, Type<TypeCategory::Logical, KIND>,
        Type<TypeCategory::Logical, KIND>, Type<TypeCategory::Logical, KIND>> {
  using Result = Type<TypeCategory::Logical, KIND>;
  using Operand = Result;
  using Base = Operation<LogicalOperation, Result, Operand, Operand>;
  CLASS_BOILERPLATE(LogicalOperation)
  LogicalOperation(
      LogicalOperator opr, const Expr<Operand> &x, const Expr<Operand> &y)
    : Base{x, y}, logicalOperator{opr} {}
  LogicalOperation(LogicalOperator opr, Expr<Operand> &&x, Expr<Operand> &&y)
    : Base{std::move(x), std::move(y)}, logicalOperator{opr} {}

  const char *Infix() const;

  LogicalOperator logicalOperator;
};

// Array constructors
template<typename RESULT> class ArrayConstructorValues;

struct ImpliedDoIndex {
  using Result = SubscriptInteger;
  bool operator==(const ImpliedDoIndex &) const;
  static constexpr int Rank() { return 0; }
  parser::CharBlock name;  // nested implied DOs must use distinct names
};

template<typename RESULT> class ImpliedDo {
public:
  using Result = RESULT;
  using Index = ResultType<ImpliedDoIndex>;
  ImpliedDo(parser::CharBlock name, Expr<Index> &&lower, Expr<Index> &&upper,
      Expr<Index> &&stride, ArrayConstructorValues<Result> &&values)
    : name_{name}, lower_{std::move(lower)}, upper_{std::move(upper)},
      stride_{std::move(stride)}, values_{std::move(values)} {}
  DEFAULT_CONSTRUCTORS_AND_ASSIGNMENTS(ImpliedDo)
  bool operator==(const ImpliedDo &) const;
  parser::CharBlock name() const { return name_; }
  Expr<Index> &lower() { return lower_.value(); }
  const Expr<Index> &lower() const { return lower_.value(); }
  Expr<Index> &upper() { return upper_.value(); }
  const Expr<Index> &upper() const { return upper_.value(); }
  Expr<Index> &stride() { return stride_.value(); }
  const Expr<Index> &stride() const { return stride_.value(); }
  ArrayConstructorValues<Result> &values() { return values_.value(); }
  const ArrayConstructorValues<Result> &values() const {
    return values_.value();
  }

private:
  parser::CharBlock name_;
  common::CopyableIndirection<Expr<Index>> lower_, upper_, stride_;
  common::CopyableIndirection<ArrayConstructorValues<Result>> values_;
};

template<typename RESULT> struct ArrayConstructorValue {
  using Result = RESULT;
  EVALUATE_UNION_CLASS_BOILERPLATE(ArrayConstructorValue)
  std::variant<Expr<Result>, ImpliedDo<Result>> u;
};

template<typename RESULT> class ArrayConstructorValues {
public:
  using Result = RESULT;
  using Values = std::vector<ArrayConstructorValue<Result>>;
  DEFAULT_CONSTRUCTORS_AND_ASSIGNMENTS(ArrayConstructorValues)
  ArrayConstructorValues() {}

  bool operator==(const ArrayConstructorValues &) const;
  static constexpr int Rank() { return 1; }
  template<typename A> common::NoLvalue<A> Push(A &&x) {
    values_.emplace_back(std::move(x));
  }

  typename Values::iterator begin() { return values_.begin(); }
  typename Values::const_iterator begin() const { return values_.begin(); }
  typename Values::iterator end() { return values_.end(); }
  typename Values::const_iterator end() const { return values_.end(); }

protected:
  Values values_;
};

// Note that there are specializations of ArrayConstructor for character
// and derived types, since they must carry additional type information,
// but that an empty ArrayConstructor can be constructed for any type
// given an expression from which such type information may be gleaned.
template<typename RESULT>
class ArrayConstructor : public ArrayConstructorValues<RESULT> {
public:
  using Result = RESULT;
  using Base = ArrayConstructorValues<Result>;
  DEFAULT_CONSTRUCTORS_AND_ASSIGNMENTS(ArrayConstructor)
  explicit ArrayConstructor(Base &&values) : Base{std::move(values)} {}
  template<typename T> explicit ArrayConstructor(const Expr<T> &) {}
  static constexpr DynamicType GetType() { return Result::GetType(); }
  std::ostream &AsFortran(std::ostream &) const;
};

template<int KIND>
class ArrayConstructor<Type<TypeCategory::Character, KIND>>
  : public ArrayConstructorValues<Type<TypeCategory::Character, KIND>> {
public:
  using Result = Type<TypeCategory::Character, KIND>;
  using Base = ArrayConstructorValues<Result>;
  CLASS_BOILERPLATE(ArrayConstructor)
  ArrayConstructor(Expr<SubscriptInteger> &&len, Base &&v)
    : Base{std::move(v)}, length_{std::move(len)} {}
  template<typename T>
  explicit ArrayConstructor(const Expr<T> &proto) : length_{proto.LEN()} {}
  bool operator==(const ArrayConstructor &) const;
  static constexpr DynamicType GetType() { return Result::GetType(); }
  std::ostream &AsFortran(std::ostream &) const;
  const Expr<SubscriptInteger> &LEN() const { return length_.value(); }

private:
  common::CopyableIndirection<Expr<SubscriptInteger>> length_;
};

template<>
class ArrayConstructor<SomeDerived>
  : public ArrayConstructorValues<SomeDerived> {
public:
  using Result = SomeDerived;
  using Base = ArrayConstructorValues<Result>;
  CLASS_BOILERPLATE(ArrayConstructor)
  ArrayConstructor(const semantics::DerivedTypeSpec &spec, Base &&v)
    : Base{std::move(v)}, derivedTypeSpec_{&spec} {}
  template<typename T>
  explicit ArrayConstructor(const Expr<T> &proto)
    : derivedTypeSpec_{GetType(proto).derived} {
    CHECK(derivedTypeSpec_ != nullptr);
  }
  bool operator==(const ArrayConstructor &) const;
  const semantics::DerivedTypeSpec &derivedTypeSpec() const {
    return *derivedTypeSpec_;
  }
  DynamicType GetType() const { return DynamicType{derivedTypeSpec()}; }
  std::ostream &AsFortran(std::ostream &) const;

private:
  const semantics::DerivedTypeSpec *derivedTypeSpec_;
};

// Expression representations for each type category.

template<int KIND>
class Expr<Type<TypeCategory::Integer, KIND>>
  : public ExpressionBase<Type<TypeCategory::Integer, KIND>> {
public:
  using Result = Type<TypeCategory::Integer, KIND>;

  EVALUATE_UNION_CLASS_BOILERPLATE(Expr)

private:
  using Conversions = std::tuple<Convert<Result, TypeCategory::Integer>,
      Convert<Result, TypeCategory::Real>>;
  using Operations = std::tuple<Parentheses<Result>, Negate<Result>,
      Add<Result>, Subtract<Result>, Multiply<Result>, Divide<Result>,
      Power<Result>, Extremum<Result>>;
  using Indices = std::conditional_t<KIND == ImpliedDoIndex::Result::kind,
      std::tuple<ImpliedDoIndex>, std::tuple<>>;
  using DescriptorInquiries =
      std::conditional_t<KIND == DescriptorInquiry::Result::kind,
          std::tuple<DescriptorInquiry>, std::tuple<>>;
  using Others = std::tuple<Constant<Result>, ArrayConstructor<Result>,
      TypeParamInquiry<KIND>, Designator<Result>, FunctionRef<Result>>;

public:
  common::TupleToVariant<common::CombineTuples<Operations, Conversions, Indices,
      DescriptorInquiries, Others>>
      u;
};

template<int KIND>
class Expr<Type<TypeCategory::Real, KIND>>
  : public ExpressionBase<Type<TypeCategory::Real, KIND>> {
public:
  using Result = Type<TypeCategory::Real, KIND>;

  EVALUATE_UNION_CLASS_BOILERPLATE(Expr)
  explicit Expr(const Scalar<Result> &x) : u{Constant<Result>{x}} {}

private:
  // N.B. Real->Complex and Complex->Real conversions are done with CMPLX
  // and part access operations (resp.).  Conversions between kinds of
  // Complex are done via decomposition to Real and reconstruction.
  using Conversions = std::variant<Convert<Result, TypeCategory::Integer>,
      Convert<Result, TypeCategory::Real>>;
  using Operations = std::variant<ComplexComponent<KIND>, Parentheses<Result>,
      Negate<Result>, Add<Result>, Subtract<Result>, Multiply<Result>,
      Divide<Result>, Power<Result>, RealToIntPower<Result>, Extremum<Result>>;
  using Others = std::variant<Constant<Result>, ArrayConstructor<Result>,
      Designator<Result>, FunctionRef<Result>>;

public:
  common::CombineVariants<Operations, Conversions, Others> u;
};

template<int KIND>
class Expr<Type<TypeCategory::Complex, KIND>>
  : public ExpressionBase<Type<TypeCategory::Complex, KIND>> {
public:
  using Result = Type<TypeCategory::Complex, KIND>;
  EVALUATE_UNION_CLASS_BOILERPLATE(Expr)
  explicit Expr(const Scalar<Result> &x) : u{Constant<Result>{x}} {}

  // Note that many COMPLEX operations are represented as REAL operations
  // over their components (viz., conversions, negation, add, and subtract).
  using Operations =
      std::variant<Parentheses<Result>, Multiply<Result>, Divide<Result>,
          Power<Result>, RealToIntPower<Result>, ComplexConstructor<KIND>>;
  using Others = std::variant<Constant<Result>, ArrayConstructor<Result>,
      Designator<Result>, FunctionRef<Result>>;

public:
  common::CombineVariants<Operations, Others> u;
};

FOR_EACH_INTEGER_KIND(extern template class Expr, )
FOR_EACH_REAL_KIND(extern template class Expr, )
FOR_EACH_COMPLEX_KIND(extern template class Expr, )

template<int KIND>
class Expr<Type<TypeCategory::Character, KIND>>
  : public ExpressionBase<Type<TypeCategory::Character, KIND>> {
public:
  using Result = Type<TypeCategory::Character, KIND>;
  EVALUATE_UNION_CLASS_BOILERPLATE(Expr)
  explicit Expr(const Scalar<Result> &x) : u{Constant<Result>{x}} {}
  explicit Expr(Scalar<Result> &&x) : u{Constant<Result>{std::move(x)}} {}

  Expr<SubscriptInteger> LEN() const;

  std::variant<Constant<Result>, ArrayConstructor<Result>, Designator<Result>,
      FunctionRef<Result>, Parentheses<Result>, Convert<Result>, Concat<KIND>,
      Extremum<Result>, SetLength<KIND>>
      u;
};

FOR_EACH_CHARACTER_KIND(extern template class Expr, )

// The Relational class template is a helper for constructing logical
// expressions with polymorphism over the cross product of the possible
// categories and kinds of comparable operands.
// Fortran defines a numeric relation with distinct types or kinds as
// first undergoing the same operand conversions that occur with the intrinsic
// addition operator.  Character relations must have the same kind.
// There are no relations between LOGICAL values.

template<typename T>
struct Relational : public Operation<Relational<T>, LogicalResult, T, T> {
  using Result = LogicalResult;
  using Base = Operation<Relational, LogicalResult, T, T>;
  using Operand = typename Base::template Operand<0>;
  static_assert(Operand::category == TypeCategory::Integer ||
      Operand::category == TypeCategory::Real ||
      Operand::category == TypeCategory::Character);
  CLASS_BOILERPLATE(Relational)
  Relational(
      RelationalOperator r, const Expr<Operand> &a, const Expr<Operand> &b)
    : Base{a, b}, opr{r} {}
  Relational(RelationalOperator r, Expr<Operand> &&a, Expr<Operand> &&b)
    : Base{std::move(a), std::move(b)}, opr{r} {}

  const char *Infix() const;

  RelationalOperator opr;
};

template<> class Relational<SomeType> {
  // COMPLEX data are compared piecewise.
  using DirectlyComparableTypes =
      common::CombineTuples<IntegerTypes, RealTypes, CharacterTypes>;

public:
  using Result = LogicalResult;
  EVALUATE_UNION_CLASS_BOILERPLATE(Relational)
  static constexpr DynamicType GetType() { return Result::GetType(); }
  int Rank() const {
    return std::visit([](const auto &x) { return x.Rank(); }, u);
  }
  std::ostream &AsFortran(std::ostream &o) const;
  common::MapTemplate<Relational, DirectlyComparableTypes> u;
};

FOR_EACH_INTEGER_KIND(extern template struct Relational, )
FOR_EACH_REAL_KIND(extern template struct Relational, )
FOR_EACH_CHARACTER_KIND(extern template struct Relational, )
extern template struct Relational<SomeType>;

// Logical expressions of a kind bigger than LogicalResult
// do not include Relational<> operations as possibilities,
// since the results of Relationals are always LogicalResult
// (kind=1).
template<int KIND>
class Expr<Type<TypeCategory::Logical, KIND>>
  : public ExpressionBase<Type<TypeCategory::Logical, KIND>> {
public:
  using Result = Type<TypeCategory::Logical, KIND>;
  EVALUATE_UNION_CLASS_BOILERPLATE(Expr)
  explicit Expr(const Scalar<Result> &x) : u{Constant<Result>{x}} {}
  explicit Expr(bool x) : u{Constant<Result>{x}} {}

private:
  using Operations = std::tuple<Convert<Result>, Parentheses<Result>, Not<KIND>,
      LogicalOperation<KIND>>;
  using Relations = std::conditional_t<KIND == LogicalResult::kind,
      std::tuple<Relational<SomeType>>, std::tuple<>>;
  using Others = std::tuple<Constant<Result>, ArrayConstructor<Result>,
      Designator<Result>, FunctionRef<Result>>;

public:
  common::TupleToVariant<common::CombineTuples<Operations, Relations, Others>>
      u;
};

FOR_EACH_LOGICAL_KIND(extern template class Expr, )

// StructureConstructor pairs a StructureConstructorValues instance
// (a map associating symbols with expressions) with a derived type
// specification.  There are two other similar classes:
//  - ArrayConstructor<SomeDerived> comprises a derived type spec &
//    zero or more instances of Expr<SomeDerived>; it has rank 1
//    but not (in the most general case) a known shape.
//  - Constant<SomeDerived> comprises a derived type spec, zero or more
//    homogeneous instances of StructureConstructorValues whose type
//    parameters and component expressions are all constant, and a
//    known shape (possibly scalar).
// StructureConstructor represents a scalar value of derived type that
// is not necessarily a constant.  It is used only as an Expr<SomeDerived>
// alternative and as the type Scalar<SomeDerived> (with an assumption
// of constant component value expressions).
class StructureConstructor {
public:
  explicit StructureConstructor(const semantics::DerivedTypeSpec &spec)
    : derivedTypeSpec_{&spec} {}
  StructureConstructor(
      const semantics::DerivedTypeSpec &, const StructureConstructorValues &);
  StructureConstructor(
      const semantics::DerivedTypeSpec &, StructureConstructorValues &&);
  CLASS_BOILERPLATE(StructureConstructor)

  const semantics::DerivedTypeSpec &derivedTypeSpec() const {
    return *derivedTypeSpec_;
  }
  StructureConstructorValues &values() { return values_; }
  const StructureConstructorValues &values() const { return values_; }

  bool operator==(const StructureConstructor &) const;

  StructureConstructorValues::iterator begin() { return values_.begin(); }
  StructureConstructorValues::const_iterator begin() const {
    return values_.begin();
  }
  StructureConstructorValues::iterator end() { return values_.end(); }
  StructureConstructorValues::const_iterator end() const {
    return values_.end();
  }

  StructureConstructor &Add(const semantics::Symbol &, Expr<SomeType> &&);
  int Rank() const { return 0; }
  DynamicType GetType() const;
  std::ostream &AsFortran(std::ostream &) const;

private:
  const semantics::DerivedTypeSpec *derivedTypeSpec_;
  StructureConstructorValues values_;
};

// An expression whose result has a derived type.
template<> class Expr<SomeDerived> : public ExpressionBase<SomeDerived> {
public:
  using Result = SomeDerived;
  EVALUATE_UNION_CLASS_BOILERPLATE(Expr)
  std::variant<Constant<Result>, ArrayConstructor<Result>, StructureConstructor,
      Designator<Result>, FunctionRef<Result>>
      u;
};

// A polymorphic expression of known intrinsic type category, but dynamic
// kind, represented as a discriminated union over Expr<Type<CAT, K>>
// for each supported kind K in the category.
template<TypeCategory CAT>
class Expr<SomeKind<CAT>> : public ExpressionBase<SomeKind<CAT>> {
public:
  using Result = SomeKind<CAT>;
  EVALUATE_UNION_CLASS_BOILERPLATE(Expr)
  int GetKind() const;
  common::MapTemplate<Expr, CategoryTypes<CAT>> u;
};

template<> class Expr<SomeCharacter> : public ExpressionBase<SomeCharacter> {
public:
  using Result = SomeCharacter;
  EVALUATE_UNION_CLASS_BOILERPLATE(Expr)
  int GetKind() const;
  Expr<SubscriptInteger> LEN() const;
  common::MapTemplate<Expr, CategoryTypes<TypeCategory::Character>> u;
};

// A variant comprising the Expr<> instantiations over SomeDerived and
// SomeKind<CATEGORY>.
using CategoryExpression = common::MapTemplate<Expr, SomeCategory>;

// BOZ literal "typeless" constants must be wide enough to hold a numeric
// value of any supported kind of INTEGER or REAL.  They must also be
// distinguishable from other integer constants, since they are permitted
// to be used in only a few situations.
using BOZLiteralConstant = typename LargestReal::Scalar::Word;

// Null pointers without MOLD= arguments are typed by context.
struct NullPointer {
  constexpr bool operator==(const NullPointer &) const { return true; }
  constexpr int Rank() const { return 0; }
};

// Procedure pointer targets are treated as if they were typeless.
// They are either procedure designators or values returned from
// function references.
using TypelessExpression = std::variant<BOZLiteralConstant, NullPointer,
    ProcedureDesignator, ProcedureRef>;

// A completely generic expression, polymorphic across all of the intrinsic type
// categories and each of their kinds.
template<> class Expr<SomeType> : public ExpressionBase<SomeType> {
public:
  using Result = SomeType;

  EVALUATE_UNION_CLASS_BOILERPLATE(Expr)

  // Owning references to these generic expressions can appear in other
  // compiler data structures (viz., the parse tree and symbol table), so
  // its destructor is externalized to reduce redundant default instances.
  ~Expr();

  template<TypeCategory CAT, int KIND>
  explicit Expr(const Expr<Type<CAT, KIND>> &x) : u{Expr<SomeKind<CAT>>{x}} {}

  template<TypeCategory CAT, int KIND>
  explicit Expr(Expr<Type<CAT, KIND>> &&x)
    : u{Expr<SomeKind<CAT>>{std::move(x)}} {}

  template<TypeCategory CAT, int KIND>
  Expr &operator=(const Expr<Type<CAT, KIND>> &x) {
    u = Expr<SomeKind<CAT>>{x};
    return *this;
  }

  template<TypeCategory CAT, int KIND>
  Expr &operator=(Expr<Type<CAT, KIND>> &&x) {
    u = Expr<SomeKind<CAT>>{std::move(x)};
    return *this;
  }

public:
  common::CombineVariants<TypelessExpression, CategoryExpression> u;
};

// This wrapper class is used, by means of a forward reference with
// an owning pointer, to cache analyzed expressions in parse tree nodes.
// v is nullopt if an error occurred during expression analysis.
struct GenericExprWrapper {
  GenericExprWrapper(std::optional<Expr<SomeType>> &&x) : v{std::move(x)} {}
  ~GenericExprWrapper();
  bool operator==(const GenericExprWrapper &) const;
  std::optional<Expr<SomeType>> v;
};

std::ostream &DerivedTypeSpecAsFortran(
    std::ostream &, const semantics::DerivedTypeSpec &);

FOR_EACH_CATEGORY_TYPE(extern template class Expr, )
FOR_EACH_TYPE_AND_KIND(extern template class ExpressionBase, )
FOR_EACH_INTRINSIC_KIND(extern template class ArrayConstructorValues, )
FOR_EACH_INTRINSIC_KIND(extern template class ArrayConstructor, )

// Template instantiations to resolve these "extern template" declarations.
#define INSTANTIATE_EXPRESSION_TEMPLATES \
  FOR_EACH_INTRINSIC_KIND(template class Expr, ) \
  FOR_EACH_CATEGORY_TYPE(template class Expr, ) \
  FOR_EACH_INTEGER_KIND(template struct Relational, ) \
  FOR_EACH_REAL_KIND(template struct Relational, ) \
  FOR_EACH_CHARACTER_KIND(template struct Relational, ) \
  template struct Relational<SomeType>; \
  FOR_EACH_TYPE_AND_KIND(template class ExpressionBase, ) \
  FOR_EACH_INTRINSIC_KIND(template class ArrayConstructorValues, ) \
  FOR_EACH_INTRINSIC_KIND(template class ArrayConstructor, )
}
#endif  // FORTRAN_EVALUATE_EXPRESSION_H_
