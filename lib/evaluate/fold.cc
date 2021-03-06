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

#include "fold.h"
#include "characteristics.h"
#include "common.h"
#include "constant.h"
#include "expression.h"
#include "host.h"
#include "int-power.h"
#include "intrinsics-library-templates.h"
#include "shape.h"
#include "tools.h"
#include "traversal.h"
#include "type.h"
#include "../common/indirection.h"
#include "../common/template.h"
#include "../common/unwrap.h"
#include "../parser/message.h"
#include "../semantics/scope.h"
#include "../semantics/symbol.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <optional>
#include <type_traits>
#include <variant>

namespace Fortran::evaluate {

// no-op base case
template<typename A>
common::IfNoLvalue<Expr<ResultType<A>>, A> FoldOperation(
    FoldingContext &, A &&x) {
  return Expr<ResultType<A>>{std::move(x)};
}

// Forward declarations of overloads, template instantiations, and template
// specializations of FoldOperation() to enable mutual recursion between them.
BaseObject FoldOperation(FoldingContext &, BaseObject &&);
Component FoldOperation(FoldingContext &, Component &&);
Triplet FoldOperation(FoldingContext &, Triplet &&);
Subscript FoldOperation(FoldingContext &, Subscript &&);
ArrayRef FoldOperation(FoldingContext &, ArrayRef &&);
CoarrayRef FoldOperation(FoldingContext &, CoarrayRef &&);
DataRef FoldOperation(FoldingContext &, DataRef &&);
Substring FoldOperation(FoldingContext &, Substring &&);
ComplexPart FoldOperation(FoldingContext &, ComplexPart &&);
template<int KIND>
Expr<Type<TypeCategory::Integer, KIND>> FoldOperation(
    FoldingContext &context, FunctionRef<Type<TypeCategory::Integer, KIND>> &&);
template<int KIND>
Expr<Type<TypeCategory::Real, KIND>> FoldOperation(
    FoldingContext &context, FunctionRef<Type<TypeCategory::Real, KIND>> &&);
template<int KIND>
Expr<Type<TypeCategory::Complex, KIND>> FoldOperation(
    FoldingContext &context, FunctionRef<Type<TypeCategory::Complex, KIND>> &&);
// TODO: Character intrinsic function folding
template<int KIND>
Expr<Type<TypeCategory::Logical, KIND>> FoldOperation(
    FoldingContext &context, FunctionRef<Type<TypeCategory::Logical, KIND>> &&);
template<typename T> Expr<T> FoldOperation(FoldingContext &, Designator<T> &&);
template<int KIND>
Expr<Type<TypeCategory::Integer, KIND>> FoldOperation(
    FoldingContext &, TypeParamInquiry<KIND> &&);
template<typename T>
Expr<T> FoldOperation(FoldingContext &, ArrayConstructor<T> &&);
Expr<SomeDerived> FoldOperation(FoldingContext &, StructureConstructor &&);

// Overloads, instantiations, and specializations of FoldOperation().

BaseObject FoldOperation(FoldingContext &, BaseObject &&object) {
  return std::move(object);
}

Component FoldOperation(FoldingContext &context, Component &&component) {
  return {FoldOperation(context, std::move(component.base())),
      component.GetLastSymbol()};
}

Triplet FoldOperation(FoldingContext &context, Triplet &&triplet) {
  return {Fold(context, triplet.lower()), Fold(context, triplet.upper()),
      Fold(context, common::Clone(triplet.stride()))};
}

Subscript FoldOperation(FoldingContext &context, Subscript &&subscript) {
  return std::visit(
      common::visitors{
          [&](IndirectSubscriptIntegerExpr &&expr) {
            expr.value() = Fold(context, std::move(expr.value()));
            return Subscript(std::move(expr));
          },
          [&](Triplet &&triplet) {
            return Subscript(FoldOperation(context, std::move(triplet)));
          },
      },
      std::move(subscript.u));
}

ArrayRef FoldOperation(FoldingContext &context, ArrayRef &&arrayRef) {
  for (Subscript &subscript : arrayRef.subscript()) {
    subscript = FoldOperation(context, std::move(subscript));
  }
  return std::visit(
      common::visitors{
          [&](const Symbol *symbol) {
            return ArrayRef{*symbol, std::move(arrayRef.subscript())};
          },
          [&](Component &&component) {
            return ArrayRef{FoldOperation(context, std::move(component)),
                std::move(arrayRef.subscript())};
          },
      },
      std::move(arrayRef.base()));
}

CoarrayRef FoldOperation(FoldingContext &context, CoarrayRef &&coarrayRef) {
  std::vector<Subscript> subscript;
  for (Subscript x : coarrayRef.subscript()) {
    subscript.emplace_back(FoldOperation(context, std::move(x)));
  }
  std::vector<Expr<SubscriptInteger>> cosubscript;
  for (Expr<SubscriptInteger> x : coarrayRef.cosubscript()) {
    cosubscript.emplace_back(Fold(context, std::move(x)));
  }
  CoarrayRef folded{std::move(coarrayRef.base()), std::move(subscript),
      std::move(cosubscript)};
  if (std::optional<Expr<SomeInteger>> stat{coarrayRef.stat()}) {
    folded.set_stat(Fold(context, std::move(*stat)));
  }
  if (std::optional<Expr<SomeInteger>> team{coarrayRef.team()}) {
    folded.set_team(
        Fold(context, std::move(*team)), coarrayRef.teamIsTeamNumber());
  }
  return folded;
}

DataRef FoldOperation(FoldingContext &context, DataRef &&dataRef) {
  return std::visit(
      common::visitors{
          [&](const Symbol *symbol) { return DataRef{*symbol}; },
          [&](auto &&x) {
            return DataRef{FoldOperation(context, std::move(x))};
          },
      },
      std::move(dataRef.u));
}

Substring FoldOperation(FoldingContext &context, Substring &&substring) {
  auto lower{Fold(context, substring.lower())};
  auto upper{Fold(context, substring.upper())};
  if (const DataRef * dataRef{substring.GetParentIf<DataRef>()}) {
    return Substring{FoldOperation(context, DataRef{*dataRef}),
        std::move(lower), std::move(upper)};
  } else {
    auto p{*substring.GetParentIf<StaticDataObject::Pointer>()};
    return Substring{std::move(p), std::move(lower), std::move(upper)};
  }
}

ComplexPart FoldOperation(FoldingContext &context, ComplexPart &&complexPart) {
  DataRef complex{complexPart.complex()};
  return ComplexPart{
      FoldOperation(context, std::move(complex)), complexPart.part()};
}

// helpers to fold intrinsic function references
// Define callable types used in a common utility that
// takes care of array and cast/conversion aspects for elemental intrinsics

template<typename TR, typename... TArgs>
using ScalarFunc = std::function<Scalar<TR>(const Scalar<TArgs> &...)>;
template<typename TR, typename... TArgs>
using ScalarFuncWithContext =
    std::function<Scalar<TR>(FoldingContext &, const Scalar<TArgs> &...)>;

template<template<typename, typename...> typename WrapperType, typename TR,
    typename... TA, std::size_t... I>
static inline Expr<TR> FoldElementalIntrinsicHelper(FoldingContext &context,
    FunctionRef<TR> &&funcRef, WrapperType<TR, TA...> func,
    std::index_sequence<I...>) {
  static_assert(
      (... && IsSpecificIntrinsicType<TA>));  // TODO derived types for MERGE?
  static_assert(sizeof...(TA) > 0);
  std::tuple<const Constant<TA> *...> args{
      UnwrapExpr<Constant<TA>>(*funcRef.arguments()[I].value().GetExpr())...};
  if ((... && (std::get<I>(args) != nullptr))) {
    // Compute the shape of the result based on shapes of arguments
    ConstantSubscripts shape;
    int rank{0};
    const ConstantSubscripts *shapes[sizeof...(TA)]{
        &std::get<I>(args)->shape()...};
    const int ranks[sizeof...(TA)]{std::get<I>(args)->Rank()...};
    for (unsigned int i{0}; i < sizeof...(TA); ++i) {
      if (ranks[i] > 0) {
        if (rank == 0) {
          rank = ranks[i];
          shape = *shapes[i];
        } else {
          if (shape != *shapes[i]) {
            // TODO: Rank compatibility was already checked but it seems to be
            // the first place where the actual shapes are checked to be the
            // same. Shouldn't this be checked elsewhere so that this is also
            // checked for non constexpr call to elemental intrinsics function?
            context.messages().Say(
                "arguments in elemental intrinsic function are not conformable"_err_en_US);
            return Expr<TR>{std::move(funcRef)};
          }
        }
      }
    }
    CHECK(rank == static_cast<int>(shape.size()));

    // Compute all the scalar values of the results
    std::vector<Scalar<TR>> results;
    if (TotalElementCount(shape) > 0) {
      ConstantSubscripts index{InitialSubscripts(rank)};
      do {
        if constexpr (std::is_same_v<WrapperType<TR, TA...>,
                          ScalarFuncWithContext<TR, TA...>>) {
          results.emplace_back(func(context,
              (ranks[I] ? std::get<I>(args)->At(index)
                        : **std::get<I>(args))...));
        } else if constexpr (std::is_same_v<WrapperType<TR, TA...>,
                                 ScalarFunc<TR, TA...>>) {
          results.emplace_back(func((ranks[I] ? std::get<I>(args)->At(index)
                                              : **std::get<I>(args))...));
        }
      } while (IncrementSubscripts(index, shape));
    }
    // Build and return constant result
    if constexpr (TR::category == TypeCategory::Character) {
      std::int64_t len{
          static_cast<std::int64_t>(results.size() ? results[0].length() : 0)};
      return Expr<TR>{Constant<TR>{len, std::move(results), std::move(shape)}};
    } else {
      return Expr<TR>{Constant<TR>{std::move(results), std::move(shape)}};
    }
  }
  return Expr<TR>{std::move(funcRef)};
}

template<typename TR, typename... TA>
static Expr<TR> FoldElementalIntrinsic(FoldingContext &context,
    FunctionRef<TR> &&funcRef, ScalarFunc<TR, TA...> func) {
  return FoldElementalIntrinsicHelper<ScalarFunc, TR, TA...>(
      context, std::move(funcRef), func, std::index_sequence_for<TA...>{});
}
template<typename TR, typename... TA>
static Expr<TR> FoldElementalIntrinsic(FoldingContext &context,
    FunctionRef<TR> &&funcRef, ScalarFuncWithContext<TR, TA...> func) {
  return FoldElementalIntrinsicHelper<ScalarFuncWithContext, TR, TA...>(
      context, std::move(funcRef), func, std::index_sequence_for<TA...>{});
}

template<typename T>
static Expr<T> *UnwrapArgument(std::optional<ActualArgument> &arg) {
  if (arg.has_value()) {
    if (Expr<SomeType> * expr{arg->GetExpr()}) {
      return UnwrapExpr<Expr<T>>(*expr);
    }
  }
  return nullptr;
}

static BOZLiteralConstant *UnwrapBozArgument(
    std::optional<ActualArgument> &arg) {
  if (auto *expr{UnwrapArgument<SomeType>(arg)}) {
    return std::get_if<BOZLiteralConstant>(&expr->u);
  } else {
    return nullptr;
  }
}

template<int KIND>
Expr<Type<TypeCategory::Integer, KIND>> FoldOperation(FoldingContext &context,
    FunctionRef<Type<TypeCategory::Integer, KIND>> &&funcRef) {
  using T = Type<TypeCategory::Integer, KIND>;
  ActualArguments &args{funcRef.arguments()};
  for (std::optional<ActualArgument> &arg : args) {
    if (auto *expr{UnwrapArgument<SomeType>(arg)}) {
      *expr = FoldOperation(context, std::move(*expr));
    }
  }
  if (auto *intrinsic{std::get_if<SpecificIntrinsic>(&funcRef.proc().u)}) {
    const std::string name{intrinsic->name};
    if (name == "abs") {
      return FoldElementalIntrinsic<T, T>(context, std::move(funcRef),
          ScalarFunc<T, T>([&context](const Scalar<T> &i) -> Scalar<T> {
            typename Scalar<T>::ValueWithOverflow j{i.ABS()};
            if (j.overflow) {
              context.messages().Say(
                  "abs(integer(kind=%d)) folding overflowed"_en_US, KIND);
            }
            return j.value;
          }));
    } else if (name == "dim") {
      return FoldElementalIntrinsic<T, T, T>(
          context, std::move(funcRef), &Scalar<T>::DIM);
    } else if (name == "dshiftl" || name == "dshiftr") {
      // convert boz
      for (int i{0}; i <= 1; ++i) {
        if (auto *x{UnwrapBozArgument(args[i])}) {
          *args[i] =
              AsGenericExpr(Fold(context, ConvertToType<T>(std::move(*x))));
        }
      }
      // Third argument can be of any kind. However, it must be smaller or equal
      // than BIT_SIZE. It can be converted to Int4 to simplify.
      using Int4 = Type<TypeCategory::Integer, 4>;
      if (auto *n{UnwrapArgument<SomeInteger>(args[2])}) {
        if (n->GetType()->kind != 4) {
          *args[2] =
              AsGenericExpr(Fold(context, ConvertToType<Int4>(std::move(*n))));
        }
      }
      const auto fptr{
          name == "dshiftl" ? &Scalar<T>::DSHIFTL : &Scalar<T>::DSHIFTR};
      return FoldElementalIntrinsic<T, T, T, Int4>(context, std::move(funcRef),
          ScalarFunc<T, T, T, Int4>(
              [&fptr](const Scalar<T> &i, const Scalar<T> &j,
                  const Scalar<Int4> &shift) -> Scalar<T> {
                return std::invoke(
                    fptr, i, j, static_cast<int>(shift.ToInt64()));
              }));
    } else if (name == "exponent") {
      if (auto *sx{UnwrapArgument<SomeReal>(args[0])}) {
        return std::visit(
            [&funcRef, &context](const auto &x) -> Expr<T> {
              using TR = typename std::decay_t<decltype(x)>::Result;
              return FoldElementalIntrinsic<T, TR>(context, std::move(funcRef),
                  &Scalar<TR>::template EXPONENT<Scalar<T>>);
            },
            sx->u);
      } else {
        common::die("exponent argument must be real");
      }
    } else if (name == "iand" || name == "ior" || name == "ieor") {
      // convert boz
      for (int i{0}; i <= 1; ++i) {
        if (auto *x{UnwrapBozArgument(args[i])}) {
          *args[i] =
              AsGenericExpr(Fold(context, ConvertToType<T>(std::move(*x))));
        }
      }
      auto fptr{&Scalar<T>::IAND};
      if (name == "iand") {  // done in fptr declaration
      } else if (name == "ior") {
        fptr = &Scalar<T>::IOR;
      } else if (name == "ieor") {
        fptr = &Scalar<T>::IEOR;
      } else {
        common::die("missing case to fold intrinsic function %s", name.c_str());
      }
      return FoldElementalIntrinsic<T, T, T>(
          context, std::move(funcRef), ScalarFunc<T, T, T>(fptr));
    } else if (name == "ibclr" || name == "ibset" || name == "ishft" ||
        name == "shifta" || name == "shiftr" || name == "shiftl") {
      // Second argument can be of any kind. However, it must be smaller or
      // equal than BIT_SIZE. It can be converted to Int4 to simplify.
      using Int4 = Type<TypeCategory::Integer, 4>;
      if (auto *n{UnwrapArgument<SomeInteger>(args[1])}) {
        if (n->GetType()->kind != 4) {
          *args[1] =
              AsGenericExpr(Fold(context, ConvertToType<Int4>(std::move(*n))));
        }
      }
      auto fptr{&Scalar<T>::IBCLR};
      if (name == "ibclr") {  // done in fprt definition
      } else if (name == "ibset") {
        fptr = &Scalar<T>::IBSET;
      } else if (name == "ibshft") {
        fptr = &Scalar<T>::ISHFT;
      } else if (name == "shifta") {
        fptr = &Scalar<T>::SHIFTA;
      } else if (name == "shiftr") {
        fptr = &Scalar<T>::SHIFTR;
      } else if (name == "shiftl") {
        fptr = &Scalar<T>::SHIFTL;
      } else {
        common::die("missing case to fold intrinsic function %s", name.c_str());
      }
      return FoldElementalIntrinsic<T, T, Int4>(context, std::move(funcRef),
          ScalarFunc<T, T, Int4>([&fptr](const Scalar<T> &i,
                                     const Scalar<Int4> &pos) -> Scalar<T> {
            return std::invoke(fptr, i, static_cast<int>(pos.ToInt64()));
          }));
    } else if (name == "int") {
      if (auto *expr{args[0].value().GetExpr()}) {
        return std::visit(
            [&](auto &&x) -> Expr<T> {
              using From = std::decay_t<decltype(x)>;
              if constexpr (std::is_same_v<From, BOZLiteralConstant> ||
                  IsNumericCategoryExpr<From>()) {
                return Fold(context, ConvertToType<T>(std::move(x)));
              }
              common::die("int() argument type not valid");
            },
            std::move(expr->u));
      }
    } else if (name == "kind") {
      if constexpr (common::HasMember<T, IntegerTypes>) {
        return Expr<T>{args[0].value().GetType()->kind};
      } else {
        common::die("kind() result not integral");
      }
    } else if (name == "leadz" || name == "trailz" || name == "poppar" ||
        name == "popcnt") {
      if (auto *sn{UnwrapArgument<SomeInteger>(args[0])}) {
        return std::visit(
            [&funcRef, &context, &name](const auto &n) -> Expr<T> {
              using TI = typename std::decay_t<decltype(n)>::Result;
              if (name == "poppar") {
                return FoldElementalIntrinsic<T, TI>(context,
                    std::move(funcRef),
                    ScalarFunc<T, TI>([](const Scalar<TI> &i) -> Scalar<T> {
                      return Scalar<T>{i.POPPAR() ? 1 : 0};
                    }));
              }
              auto fptr{&Scalar<TI>::LEADZ};
              if (name == "leadz") {  // done in fprt definition
              } else if (name == "trailz") {
                fptr = &Scalar<TI>::TRAILZ;
              } else if (name == "popcnt") {
                fptr = &Scalar<TI>::POPCNT;
              } else {
                common::die(
                    "missing case to fold intrinsic function %s", name.c_str());
              }
              return FoldElementalIntrinsic<T, TI>(context, std::move(funcRef),
                  ScalarFunc<T, TI>([&fptr](const Scalar<TI> &i) -> Scalar<T> {
                    return Scalar<T>{std::invoke(fptr, i)};
                  }));
            },
            sn->u);
      } else {
        common::die("leadz argument must be integer");
      }
    } else if (name == "len") {
      if (auto *charExpr{UnwrapArgument<SomeCharacter>(args[0])}) {
        return std::visit(
            [&](auto &kx) { return Fold(context, ConvertToType<T>(kx.LEN())); },
            charExpr->u);
      } else {
        common::die("len() argument must be of character type");
      }
    } else if (name == "maskl" || name == "maskr") {
      // Argument can be of any kind but value has to be smaller than bit_size.
      // It can be safely converted to Int4 to simplify.
      using Int4 = Type<TypeCategory::Integer, 4>;
      if (auto *n{UnwrapArgument<SomeInteger>(args[0])}) {
        if (n->GetType()->kind != 4) {
          *args[0] =
              AsGenericExpr(Fold(context, ConvertToType<Int4>(std::move(*n))));
        }
      }
      const auto fptr{name == "maskl" ? &Scalar<T>::MASKL : &Scalar<T>::MASKR};
      return FoldElementalIntrinsic<T, Int4>(context, std::move(funcRef),
          ScalarFunc<T, Int4>([&fptr](const Scalar<Int4> &places) -> Scalar<T> {
            return fptr(static_cast<int>(places.ToInt64()));
          }));
    } else if (name == "merge_bits") {
      // convert boz
      for (int i{0}; i <= 2; ++i) {
        if (auto *x{UnwrapBozArgument(args[i])}) {
          *args[i] =
              AsGenericExpr(Fold(context, ConvertToType<T>(std::move(*x))));
        }
      }
      return FoldElementalIntrinsic<T, T, T, T>(
          context, std::move(funcRef), &Scalar<T>::MERGE_BITS);
    } else if (name == "rank") {
      // TODO assumed-rank dummy argument
      return Expr<T>{args[0].value().Rank()};
    } else if (name == "shape") {
      if (auto shape{GetShape(context, args[0].value())}) {
        if (auto shapeExpr{AsExtentArrayExpr(*shape)}) {
          return Fold(context, ConvertToType<T>(std::move(*shapeExpr)));
        }
      }
    } else if (name == "size") {
      if (auto shape{GetShape(context, args[0].value())}) {
        if (auto &dimArg{args[1]}) {  // DIM= is present, get one extent
          if (auto *expr{dimArg->GetExpr()}) {
            if (auto dim{ToInt64(*expr)}) {
              std::int64_t rank = shape->size();
              if (*dim >= 1 && *dim <= rank) {
                if (auto &extent{shape->at(*dim - 1)}) {
                  return Fold(context, ConvertToType<T>(std::move(*extent)));
                }
              } else {
                context.messages().Say(
                    "size(array,dim=%jd) dimension is out of range for rank-%d array"_en_US,
                    static_cast<std::intmax_t>(*dim), static_cast<int>(rank));
              }
            }
          }
        } else if (auto extents{
                       common::AllElementsPresent(std::move(*shape))}) {
          // DIM= is absent; compute PRODUCT(SHAPE())
          ExtentExpr product{1};
          for (auto &&extent : std::move(*extents)) {
            product = std::move(product) * std::move(extent);
          }
          return Expr<T>{ConvertToType<T>(Fold(context, std::move(product)))};
        }
      }
    }
    // TODO:
    // ceiling, count, cshift, dot_product, eoshift,
    // findloc, floor, iachar, iall, iany, iparity, ibits, ichar, image_status,
    // index, ishftc, lbound, len_trim, matmul, max, maxloc, maxval, merge, min,
    // minloc, minval, mod, modulo, nint, not, pack, product, reduce, reshape,
    // scan, selected_char_kind, selected_int_kind, selected_real_kind,
    // sign, spread, sum, transfer, transpose, ubound, unpack, verify
  }
  return Expr<T>{std::move(funcRef)};
}

template<int KIND>
Expr<Type<TypeCategory::Real, KIND>> ToReal(
    FoldingContext &context, Expr<SomeType> &&expr) {
  using Result = Type<TypeCategory::Real, KIND>;
  std::optional<Expr<Result>> result;
  std::visit(
      [&](auto &&x) {
        using From = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<From, BOZLiteralConstant>) {
          // Move the bits without any integer->real conversion
          From original{x};
          result = ConvertToType<Result>(std::move(x));
          const auto *constant{UnwrapExpr<Constant<Result>>(*result)};
          CHECK(constant != nullptr);
          const Scalar<Result> &real{**constant};
          From converted{From::ConvertUnsigned(real.RawBits()).value};
          if (!(original == converted)) {  // C1601
            context.messages().Say(
                "Nonzero bits truncated from BOZ literal constant in REAL intrinsic"_en_US);
          }
        } else if constexpr (IsNumericCategoryExpr<From>()) {
          result = Fold(context, ConvertToType<Result>(std::move(x)));
        } else {
          common::die("ToReal: bad argument expression");
        }
      },
      std::move(expr.u));
  return result.value();
}

template<int KIND>
Expr<Type<TypeCategory::Real, KIND>> FoldOperation(FoldingContext &context,
    FunctionRef<Type<TypeCategory::Real, KIND>> &&funcRef) {
  using T = Type<TypeCategory::Real, KIND>;
  using ComplexT = Type<TypeCategory::Complex, KIND>;
  ActualArguments &args{funcRef.arguments()};
  for (std::optional<ActualArgument> &arg : args) {
    if (arg.has_value()) {
      if (auto *expr{arg->GetExpr()}) {
        *expr = FoldOperation(context, std::move(*expr));
      }
    }
  }
  if (auto *intrinsic{std::get_if<SpecificIntrinsic>(&funcRef.proc().u)}) {
    const std::string name{intrinsic->name};
    if (name == "acos" || name == "acosh" || name == "asin" ||
        name == "asinh" || (name == "atan" && args.size() == 1) ||
        name == "atanh" || name == "bessel_j0" || name == "bessel_j1" ||
        name == "bessel_y0" || name == "bessel_y1" || name == "cos" ||
        name == "cosh" || name == "erf" || name == "erfc" ||
        name == "erfc_scaled" || name == "exp" || name == "gamma" ||
        name == "log" || name == "log10" || name == "log_gamma" ||
        name == "sin" || name == "sinh" || name == "sqrt" || name == "tan" ||
        name == "tanh") {
      CHECK(args.size() == 1);
      if (auto callable{context.hostIntrinsicsLibrary()
                            .GetHostProcedureWrapper<Scalar, T, T>(name)}) {
        return FoldElementalIntrinsic<T, T>(
            context, std::move(funcRef), *callable);
      } else {
        context.messages().Say(
            "%s(real(kind=%d)) cannot be folded on host"_en_US, name, KIND);
      }
    }
    if (name == "atan" || name == "atan2" || name == "hypot" || name == "mod") {
      std::string localName{name == "atan2" ? "atan" : name};
      CHECK(args.size() == 2);
      if (auto callable{
              context.hostIntrinsicsLibrary()
                  .GetHostProcedureWrapper<Scalar, T, T, T>(localName)}) {
        return FoldElementalIntrinsic<T, T, T>(
            context, std::move(funcRef), *callable);
      } else {
        context.messages().Say(
            "%s(real(kind=%d), real(kind%d)) cannot be folded on host"_en_US,
            name, KIND, KIND);
      }
    } else if (name == "bessel_jn" || name == "bessel_yn") {
      if (args.size() == 2) {  // elemental
        // runtime functions use int arg
        using Int4 = Type<TypeCategory::Integer, 4>;
        if (auto *n{UnwrapArgument<SomeInteger>(args[0])}) {
          if (n->GetType()->kind != 4) {
            *args[0] = AsGenericExpr(
                Fold(context, ConvertToType<Int4>(std::move(*n))));
          }
        }
        if (auto callable{
                context.hostIntrinsicsLibrary()
                    .GetHostProcedureWrapper<Scalar, T, Int4, T>(name)}) {
          return FoldElementalIntrinsic<T, Int4, T>(
              context, std::move(funcRef), *callable);
        } else {
          context.messages().Say(
              "%s(integer(kind=4), real(kind=%d)) cannot be folded on host"_en_US,
              name, KIND);
        }
      }
    } else if (name == "abs") {
      // Argument can be complex or real
      if (auto *x{UnwrapArgument<SomeReal>(args[0])}) {
        return FoldElementalIntrinsic<T, T>(
            context, std::move(funcRef), &Scalar<T>::ABS);
      } else if (auto *z{UnwrapArgument<SomeComplex>(args[0])}) {
        if (auto callable{
                context.hostIntrinsicsLibrary()
                    .GetHostProcedureWrapper<Scalar, T, ComplexT>("abs")}) {
          return FoldElementalIntrinsic<T, ComplexT>(
              context, std::move(funcRef), *callable);
        } else {
          context.messages().Say(
              "abs(complex(kind=%d)) cannot be folded on host"_en_US, KIND);
        }
      } else {
        common::die(" unexpected argument type inside abs");
      }
    } else if (name == "aimag") {
      return FoldElementalIntrinsic<T, ComplexT>(
          context, std::move(funcRef), &Scalar<ComplexT>::AIMAG);
    } else if (name == "aint") {
      // Convert argument to the requested kind before calling aint
      if (auto *x{UnwrapArgument<SomeReal>(args[0])}) {
        if (!(x->GetType()->kind == T::kind)) {
          *args[0] =
              AsGenericExpr(Fold(context, ConvertToType<T>(std::move(*x))));
        }
      }
      return FoldElementalIntrinsic<T, T>(context, std::move(funcRef),
          ScalarFunc<T, T>([&name, &context](const Scalar<T> &x) -> Scalar<T> {
            ValueWithRealFlags<Scalar<T>> y{x.AINT()};
            if (y.flags.test(RealFlag::Overflow)) {
              context.messages().Say(
                  "%s intrinsic folding overflow"_en_US, name);
            }
            return y.value;
          }));
    } else if (name == "dprod") {
      if (auto *x{UnwrapArgument<SomeReal>(args[0])}) {
        if (auto *y{UnwrapArgument<SomeReal>(args[1])}) {
          return Fold(context,
              Expr<T>{Multiply<T>{ConvertToType<T>(std::move(*x)),
                  ConvertToType<T>(std::move(*y))}});
        }
      }
      common::die("Wrong argument type in dprod()");
    } else if (name == "epsilon") {
      return Expr<T>{Constant<T>{Scalar<T>::EPSILON()}};
    } else if (name == "real") {
      if (auto *expr{args[0].value().GetExpr()}) {
        return ToReal<KIND>(context, std::move(*expr));
      }
    }
    // TODO: anint, cshift, dim, dot_product, eoshift, fraction, huge, matmul,
    // max, maxval, merge, min, minval, modulo, nearest, norm2, pack, product,
    // reduce, reshape, rrspacing, scale, set_exponent, sign, spacing, spread,
    // sum, tiny, transfer, transpose, unpack, bessel_jn (transformational) and
    // bessel_yn (transformational)
  }
  return Expr<T>{std::move(funcRef)};
}

template<int KIND>
Expr<Type<TypeCategory::Complex, KIND>> FoldOperation(FoldingContext &context,
    FunctionRef<Type<TypeCategory::Complex, KIND>> &&funcRef) {
  using T = Type<TypeCategory::Complex, KIND>;
  ActualArguments &args{funcRef.arguments()};
  for (std::optional<ActualArgument> &arg : args) {
    if (arg.has_value()) {
      if (auto *expr{arg->GetExpr()}) {
        *expr = FoldOperation(context, std::move(*expr));
      }
    }
  }
  if (auto *intrinsic{std::get_if<SpecificIntrinsic>(&funcRef.proc().u)}) {
    const std::string name{intrinsic->name};
    if (name == "acos" || name == "acosh" || name == "asin" ||
        name == "asinh" || name == "atan" || name == "atanh" || name == "cos" ||
        name == "cosh" || name == "exp" || name == "log" || name == "sin" ||
        name == "sinh" || name == "sqrt" || name == "tan" || name == "tanh") {
      if (auto callable{context.hostIntrinsicsLibrary()
                            .GetHostProcedureWrapper<Scalar, T, T>(name)}) {
        return FoldElementalIntrinsic<T, T>(
            context, std::move(funcRef), *callable);
      } else {
        context.messages().Say(
            "%s(complex(kind=%d)) cannot be folded on host"_en_US, name, KIND);
      }
    } else if (name == "conjg") {
      return FoldElementalIntrinsic<T, T>(
          context, std::move(funcRef), &Scalar<T>::CONJG);
    } else if (name == "cmplx") {
      if (args.size() == 2) {
        if (auto *x{UnwrapArgument<SomeComplex>(args[0])}) {
          return Fold(context, ConvertToType<T>(std::move(*x)));
        } else {
          common::die("x must be complex in cmplx(x[, kind])");
        }
      } else {
        CHECK(args.size() == 3);
        using Part = typename T::Part;
        Expr<SomeType> re{std::move(*args[0].value().GetExpr())};
        Expr<SomeType> im{args[1].has_value()
                ? std::move(*args[1].value().GetExpr())
                : AsGenericExpr(Constant<Part>{Scalar<Part>{}})};
        return Fold(context,
            Expr<T>{
                ComplexConstructor<KIND>{ToReal<KIND>(context, std::move(re)),
                    ToReal<KIND>(context, std::move(im))}});
      }
    }
    // TODO: cshift, dot_product, eoshift, matmul, merge, pack, product,
    // reduce, reshape, spread, sum, transfer, transpose, unpack
  }
  return Expr<T>{std::move(funcRef)};
}

template<int KIND>
Expr<Type<TypeCategory::Logical, KIND>> FoldOperation(FoldingContext &context,
    FunctionRef<Type<TypeCategory::Logical, KIND>> &&funcRef) {
  using T = Type<TypeCategory::Logical, KIND>;
  ActualArguments &args{funcRef.arguments()};
  for (std::optional<ActualArgument> &arg : args) {
    if (arg.has_value()) {
      if (auto *expr{arg->GetExpr()}) {
        *expr = FoldOperation(context, std::move(*expr));
      }
    }
  }
  if (auto *intrinsic{std::get_if<SpecificIntrinsic>(&funcRef.proc().u)}) {
    std::string name{intrinsic->name};
    if (name == "bge" || name == "bgt" || name == "ble" || name == "blt") {
      using LargestInt = Type<TypeCategory::Integer, 16>;
      static_assert(std::is_same_v<Scalar<LargestInt>, BOZLiteralConstant>);
      // Arguments do not have to be of the same integer type. Convert all
      // arguments to the biggest integer type before comparing them to
      // simplify.
      for (int i{0}; i <= 1; ++i) {
        if (auto *x{UnwrapArgument<SomeInteger>(args[i])}) {
          *args[i] = AsGenericExpr(
              Fold(context, ConvertToType<LargestInt>(std::move(*x))));
        } else if (auto *x{UnwrapBozArgument(args[i])}) {
          *args[i] = AsGenericExpr(Constant<LargestInt>{std::move(*x)});
        }
      }
      auto fptr{&Scalar<LargestInt>::BGE};
      if (name == "bge") {  // done in fptr declaration
      } else if (name == "bgt") {
        fptr = &Scalar<LargestInt>::BGT;
      } else if (name == "ble") {
        fptr = &Scalar<LargestInt>::BLE;
      } else if (name == "blt") {
        fptr = &Scalar<LargestInt>::BLT;
      } else {
        common::die("missing case to fold intrinsic function %s", name.c_str());
      }
      return FoldElementalIntrinsic<T, LargestInt, LargestInt>(context,
          std::move(funcRef),
          ScalarFunc<T, LargestInt, LargestInt>(
              [&fptr](
                  const Scalar<LargestInt> &i, const Scalar<LargestInt> &j) {
                return Scalar<T>{std::invoke(fptr, i, j)};
              }));
    }
    // TODO: all, any, btest, cshift, dot_product, eoshift, is_iostat_end,
    // is_iostat_eor, lge, lgt, lle, llt, logical, matmul, merge, out_of_range,
    // pack, parity, reduce, reshape, spread, transfer, transpose, unpack
  }
  return Expr<T>{std::move(funcRef)};
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, Designator<T> &&designator) {
  if constexpr (T::category == TypeCategory::Character) {
    if (auto *substring{common::Unwrap<Substring>(designator.u)}) {
      if (std::optional<Expr<SomeCharacter>> folded{substring->Fold(context)}) {
        if (auto value{GetScalarConstantValue<T>(*folded)}) {
          return Expr<T>{*value};
        }
      }
      if (auto length{ToInt64(Fold(context, substring->LEN()))}) {
        if (*length == 0) {
          return Expr<T>{Constant<T>{Scalar<T>{}}};
        }
      }
    }
  }
  return std::visit(
      common::visitors{
          [&](const Symbol *symbol) { return Expr<T>{std::move(designator)}; },
          [&](auto &&x) {
            return Expr<T>{Designator<T>{FoldOperation(context, std::move(x))}};
          },
      },
      std::move(designator.u));
}

// Array constructor folding

Expr<ImpliedDoIndex::Result> FoldOperation(
    FoldingContext &context, ImpliedDoIndex &&iDo) {
  if (std::optional<std::int64_t> value{context.GetImpliedDo(iDo.name)}) {
    return Expr<ImpliedDoIndex::Result>{*value};
  } else {
    return Expr<ImpliedDoIndex::Result>{std::move(iDo)};
  }
}

template<typename T> class ArrayConstructorFolder {
public:
  explicit ArrayConstructorFolder(const FoldingContext &c) : context_{c} {}

  Expr<T> FoldArray(ArrayConstructor<T> &&array) {
    if (FoldArray(array)) {
      auto n{static_cast<std::int64_t>(elements_.size())};
      if constexpr (std::is_same_v<T, SomeDerived>) {
        return Expr<T>{Constant<T>{array.derivedTypeSpec(),
            std::move(elements_), ConstantSubscripts{n}}};
      } else if constexpr (T::category == TypeCategory::Character) {
        auto length{Fold(context_, common::Clone(array.LEN()))};
        if (std::optional<std::int64_t> lengthValue{ToInt64(length)}) {
          return Expr<T>{Constant<T>{
              *lengthValue, std::move(elements_), ConstantSubscripts{n}}};
        }
      } else {
        return Expr<T>{
            Constant<T>{std::move(elements_), ConstantSubscripts{n}}};
      }
    }
    return Expr<T>{std::move(array)};
  }

private:
  bool FoldArray(const common::CopyableIndirection<Expr<T>> &expr) {
    Expr<T> folded{Fold(context_, common::Clone(expr.value()))};
    if (auto *c{UnwrapExpr<Constant<T>>(folded)}) {
      // Copy elements in Fortran array element order
      ConstantSubscripts shape{c->shape()};
      int rank{c->Rank()};
      ConstantSubscripts index(shape.size(), 1);
      for (std::size_t n{c->size()}; n-- > 0;) {
        if constexpr (std::is_same_v<T, SomeDerived>) {
          elements_.emplace_back(c->derivedTypeSpec(), c->At(index));
        } else {
          elements_.emplace_back(c->At(index));
        }
        for (int d{0}; d < rank; ++d) {
          if (++index[d] <= shape[d]) {
            break;
          }
          index[d] = 1;
        }
      }
      return true;
    } else {
      return false;
    }
  }
  bool FoldArray(const ImpliedDo<T> &iDo) {
    Expr<SubscriptInteger> lower{
        Fold(context_, Expr<SubscriptInteger>{iDo.lower()})};
    Expr<SubscriptInteger> upper{
        Fold(context_, Expr<SubscriptInteger>{iDo.upper()})};
    Expr<SubscriptInteger> stride{
        Fold(context_, Expr<SubscriptInteger>{iDo.stride()})};
    std::optional<std::int64_t> start{ToInt64(lower)}, end{ToInt64(upper)},
        step{ToInt64(stride)};
    if (start.has_value() && end.has_value() && step.has_value()) {
      if (*step == 0) {
        return false;
      }
      bool result{true};
      std::int64_t &j{context_.StartImpliedDo(iDo.name(), *start)};
      if (*step > 0) {
        for (; j <= *end; j += *step) {
          result &= FoldArray(iDo.values());
        }
      } else {
        for (; j >= *end; j += *step) {
          result &= FoldArray(iDo.values());
        }
      }
      context_.EndImpliedDo(iDo.name());
      return result;
    } else {
      return false;
    }
  }
  bool FoldArray(const ArrayConstructorValue<T> &x) {
    return std::visit([&](const auto &y) { return FoldArray(y); }, x.u);
  }
  bool FoldArray(const ArrayConstructorValues<T> &xs) {
    for (const auto &x : xs) {
      if (!FoldArray(x)) {
        return false;
      }
    }
    return true;
  }

  FoldingContext context_;
  std::vector<Scalar<T>> elements_;
};

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, ArrayConstructor<T> &&array) {
  ArrayConstructorFolder<T> folder{context};
  Expr<T> result{folder.FoldArray(std::move(array))};
  return result;
}

Expr<SomeDerived> FoldOperation(
    FoldingContext &context, StructureConstructor &&structure) {
  StructureConstructor result{structure.derivedTypeSpec()};
  for (auto &&[symbol, value] : std::move(structure)) {
    result.Add(*symbol, Fold(context, std::move(value.value())));
  }
  return Expr<SomeDerived>{Constant<SomeDerived>{result}};
}

// Substitute a bare type parameter reference with its value if it has one now
template<int KIND>
Expr<Type<TypeCategory::Integer, KIND>> FoldOperation(
    FoldingContext &context, TypeParamInquiry<KIND> &&inquiry) {
  using IntKIND = Type<TypeCategory::Integer, KIND>;
  if (Component * component{common::Unwrap<Component>(inquiry.base())}) {
    return Expr<IntKIND>{TypeParamInquiry<KIND>{
        FoldOperation(context, std::move(*component)), inquiry.parameter()}};
  }
  if (context.pdtInstance() != nullptr &&
      std::get<const Symbol *>(inquiry.base()) == nullptr) {
    // "bare" type parameter: replace with actual value
    const semantics::Scope *scope{context.pdtInstance()->scope()};
    CHECK(scope != nullptr);
    auto iter{scope->find(inquiry.parameter().name())};
    if (iter != scope->end()) {
      const Symbol &symbol{*iter->second};
      const auto *details{symbol.detailsIf<semantics::TypeParamDetails>()};
      if (details && details->init().has_value()) {
        Expr<SomeInteger> expr{*details->init()};
        return Fold(context,
            Expr<IntKIND>{
                Convert<IntKIND, TypeCategory::Integer>(std::move(expr))});
      }
    } else if (const auto *value{context.pdtInstance()->FindParameter(
                   inquiry.parameter().name())}) {
      // Parameter of a parent derived type; these are saved in the spec.
      if (value->isExplicit()) {
        return Fold(context,
            Expr<IntKIND>{Convert<IntKIND, TypeCategory::Integer>(
                Expr<SomeInteger>{value->GetExplicit().value()})});
      }
    }
  }
  return Expr<IntKIND>{std::move(inquiry)};
}

// Array operation elemental application: When all operands to an operation
// are constant arrays, array constructors without any implied DO loops,
// &/or expanded scalars, pull the operation "into" the array result by
// applying it in an elementwise fashion.  For example, [A,1]+[B,2]
// is rewritten into [A+B,1+2] and then partially folded to [A+B,3].

// If possible, restructures an array expression into an array constructor
// that comprises a "flat" ArrayConstructorValues with no implied DO loops.
template<typename T>
bool ArrayConstructorIsFlat(const ArrayConstructorValues<T> &values) {
  for (const ArrayConstructorValue<T> &x : values) {
    if (!std::holds_alternative<Expr<T>>(x.u)) {
      return false;
    }
  }
  return true;
}

template<typename T>
std::optional<Expr<T>> AsFlatArrayConstructor(const Expr<T> &expr) {
  if (const auto *c{UnwrapExpr<Constant<T>>(expr)}) {
    ArrayConstructor<T> result{expr};
    if (c->size() > 0) {
      ConstantSubscripts at{InitialSubscripts(c->shape())};
      do {
        result.Push(Expr<T>{Constant<T>{c->At(at)}});
      } while (IncrementSubscripts(at, c->shape()));
    }
    return std::make_optional<Expr<T>>(std::move(result));
  } else if (const auto *a{UnwrapExpr<ArrayConstructor<T>>(expr)}) {
    if (ArrayConstructorIsFlat(*a)) {
      return std::make_optional<Expr<T>>(expr);
    }
  } else if (const auto *p{UnwrapExpr<Parentheses<T>>(expr)}) {
    return AsFlatArrayConstructor(Expr<T>{p->left()});
  }
  return std::nullopt;
}

template<TypeCategory CAT>
std::optional<Expr<SomeKind<CAT>>> AsFlatArrayConstructor(
    const Expr<SomeKind<CAT>> &expr) {
  return std::visit(
      [&](const auto &kindExpr) -> std::optional<Expr<SomeKind<CAT>>> {
        if (auto flattened{AsFlatArrayConstructor(kindExpr)}) {
          return Expr<SomeKind<CAT>>{std::move(*flattened)};
        } else {
          return std::nullopt;
        }
      },
      expr.u);
}

// FromArrayConstructor is a subroutine for MapOperation() below.
// Given a flat ArrayConstructor<T> and a shape, it wraps the array
// into an Expr<T>, folds it, and returns the resulting wrapped
// array constructor or constant array value.
template<typename T>
Expr<T> FromArrayConstructor(FoldingContext &context,
    ArrayConstructor<T> &&values, std::optional<ConstantSubscripts> &&shape) {
  Expr<T> result{Fold(context, Expr<T>{std::move(values)})};
  if (shape.has_value()) {
    if (auto *constant{UnwrapExpr<Constant<T>>(result)}) {
      constant->shape() = std::move(*shape);
    } else {
      auto resultShape{GetShape(context, result)};
      CHECK(resultShape.has_value());
      auto constantShape{AsConstantShape(*resultShape)};
      CHECK(constantShape.has_value());
      CHECK(*shape == AsConstantExtents(*constantShape));
    }
  }
  return result;
}

// MapOperation is a utility for various specializations of ApplyElementwise()
// that follow.  Given one or two flat ArrayConstructor<OPERAND> (wrapped in an
// Expr<OPERAND>) for some specific operand type(s), apply a given function f
// to each of their corresponding elements to produce a flat
// ArrayConstructor<RESULT> (wrapped in an Expr<RESULT>).
// Preserves shape.

// Unary case
template<typename RESULT, typename OPERAND>
Expr<RESULT> MapOperation(FoldingContext &context,
    std::function<Expr<RESULT>(Expr<OPERAND> &&)> &&f, const Shape &shape,
    Expr<OPERAND> &&values) {
  ArrayConstructor<RESULT> result{values};
  if constexpr (common::HasMember<OPERAND, AllIntrinsicCategoryTypes>) {
    std::visit(
        [&](auto &&kindExpr) {
          using kindType = ResultType<decltype(kindExpr)>;
          auto &aConst{std::get<ArrayConstructor<kindType>>(kindExpr.u)};
          for (auto &acValue : aConst) {
            auto &scalar{std::get<Expr<kindType>>(acValue.u)};
            result.Push(
                FoldOperation(context, f(Expr<OPERAND>{std::move(scalar)})));
          }
        },
        std::move(values.u));
  } else {
    auto &aConst{std::get<ArrayConstructor<OPERAND>>(values.u)};
    for (auto &acValue : aConst) {
      auto &scalar{std::get<Expr<OPERAND>>(acValue.u)};
      result.Push(FoldOperation(context, f(std::move(scalar))));
    }
  }
  return FromArrayConstructor(
      context, std::move(result), AsConstantExtents(shape));
}

// array * array case
template<typename RESULT, typename LEFT, typename RIGHT>
Expr<RESULT> MapOperation(FoldingContext &context,
    std::function<Expr<RESULT>(Expr<LEFT> &&, Expr<RIGHT> &&)> &&f,
    const Shape &shape, Expr<LEFT> &&leftValues, Expr<RIGHT> &&rightValues) {
  ArrayConstructor<RESULT> result{leftValues};
  auto &leftArrConst{std::get<ArrayConstructor<LEFT>>(leftValues.u)};
  if constexpr (common::HasMember<RIGHT, AllIntrinsicCategoryTypes>) {
    std::visit(
        [&](auto &&kindExpr) {
          using kindType = ResultType<decltype(kindExpr)>;

          auto &rightArrConst{std::get<ArrayConstructor<kindType>>(kindExpr.u)};
          auto rightIter{rightArrConst.begin()};
          for (auto &leftValue : leftArrConst) {
            CHECK(rightIter != rightArrConst.end());
            auto &leftScalar{std::get<Expr<LEFT>>(leftValue.u)};
            auto &rightScalar{std::get<Expr<kindType>>(rightIter->u)};
            result.Push(FoldOperation(context,
                f(std::move(leftScalar), Expr<RIGHT>{std::move(rightScalar)})));
            ++rightIter;
          }
        },
        std::move(rightValues.u));
  } else {
    auto &rightArrConst{std::get<ArrayConstructor<RIGHT>>(rightValues.u)};
    auto rightIter{rightArrConst.begin()};
    for (auto &leftValue : leftArrConst) {
      CHECK(rightIter != rightArrConst.end());
      auto &leftScalar{std::get<Expr<LEFT>>(leftValue.u)};
      auto &rightScalar{std::get<Expr<RIGHT>>(rightIter->u)};
      result.Push(FoldOperation(
          context, f(std::move(leftScalar), std::move(rightScalar))));
      ++rightIter;
    }
  }
  return FromArrayConstructor(
      context, std::move(result), AsConstantExtents(shape));
}

// array * scalar case
template<typename RESULT, typename LEFT, typename RIGHT>
Expr<RESULT> MapOperation(FoldingContext &context,
    std::function<Expr<RESULT>(Expr<LEFT> &&, Expr<RIGHT> &&)> &&f,
    const Shape &shape, Expr<LEFT> &&leftValues,
    const Expr<RIGHT> &rightScalar) {
  ArrayConstructor<RESULT> result{leftValues};
  auto &leftArrConst{std::get<ArrayConstructor<LEFT>>(leftValues.u)};
  for (auto &leftValue : leftArrConst) {
    auto &leftScalar{std::get<Expr<LEFT>>(leftValue.u)};
    result.Push(FoldOperation(
        context, f(std::move(leftScalar), Expr<RIGHT>{rightScalar})));
  }
  return FromArrayConstructor(
      context, std::move(result), AsConstantExtents(shape));
}

// scalar * array case
template<typename RESULT, typename LEFT, typename RIGHT>
Expr<RESULT> MapOperation(FoldingContext &context,
    std::function<Expr<RESULT>(Expr<LEFT> &&, Expr<RIGHT> &&)> &&f,
    const Shape &shape, const Expr<LEFT> &leftScalar,
    Expr<RIGHT> &&rightValues) {
  ArrayConstructor<RESULT> result{leftScalar};
  if constexpr (common::HasMember<RIGHT, AllIntrinsicCategoryTypes>) {
    std::visit(
        [&](auto &&kindExpr) {
          using kindType = ResultType<decltype(kindExpr)>;
          auto &rightArrConst{std::get<ArrayConstructor<kindType>>(kindExpr.u)};
          for (auto &rightValue : rightArrConst) {
            auto &rightScalar{std::get<Expr<kindType>>(rightValue.u)};
            result.Push(FoldOperation(context,
                f(Expr<LEFT>{leftScalar},
                    Expr<RIGHT>{std::move(rightScalar)})));
          }
        },
        std::move(rightValues.u));
  } else {
    auto &rightArrConst{std::get<ArrayConstructor<RIGHT>>(rightValues.u)};
    for (auto &rightValue : rightArrConst) {
      auto &rightScalar{std::get<Expr<RIGHT>>(rightValue.u)};
      result.Push(FoldOperation(
          context, f(Expr<LEFT>{leftScalar}, std::move(rightScalar))));
    }
  }
  return FromArrayConstructor(
      context, std::move(result), AsConstantExtents(shape));
}

// ApplyElementwise() recursively folds the operand expression(s) of an
// operation, then attempts to apply the operation to the (corresponding)
// scalar element(s) of those operands.  Returns std::nullopt for scalars
// or unlinearizable operands.
template<typename DERIVED, typename RESULT, typename OPERAND>
auto ApplyElementwise(FoldingContext &context,
    Operation<DERIVED, RESULT, OPERAND> &operation,
    std::function<Expr<RESULT>(Expr<OPERAND> &&)> &&f)
    -> std::optional<Expr<RESULT>> {
  auto &expr{operation.left()};
  expr = Fold(context, std::move(expr));
  if (expr.Rank() > 0) {
    if (std::optional<Shape> shape{GetShape(context, expr)}) {
      if (auto values{AsFlatArrayConstructor(expr)}) {
        return MapOperation(context, std::move(f), *shape, std::move(*values));
      }
    }
  }
  return std::nullopt;
}

template<typename DERIVED, typename RESULT, typename OPERAND>
auto ApplyElementwise(
    FoldingContext &context, Operation<DERIVED, RESULT, OPERAND> &operation)
    -> std::optional<Expr<RESULT>> {
  return ApplyElementwise(context, operation,
      std::function<Expr<RESULT>(Expr<OPERAND> &&)>{
          [](Expr<OPERAND> &&operand) {
            return Expr<RESULT>{DERIVED{std::move(operand)}};
          }});
}

// Predicate: is a scalar expression suitable for naive scalar expansion
// in the flattening of an array expression?
// TODO: capture such scalar expansions in temporaries, flatten everything
struct UnexpandabilityFindingVisitor : public virtual VisitorBase<bool> {
  using Result = bool;
  explicit UnexpandabilityFindingVisitor(int) { result() = false; }
  template<typename T> void Handle(FunctionRef<T> &) { Return(true); }
  template<typename T> void Handle(CoarrayRef &) { Return(true); }
};

template<typename T> bool IsExpandableScalar(const Expr<T> &expr) {
  return Visitor<UnexpandabilityFindingVisitor>{0}.Traverse(expr);
}

template<typename DERIVED, typename RESULT, typename LEFT, typename RIGHT>
auto ApplyElementwise(FoldingContext &context,
    Operation<DERIVED, RESULT, LEFT, RIGHT> &operation,
    std::function<Expr<RESULT>(Expr<LEFT> &&, Expr<RIGHT> &&)> &&f)
    -> std::optional<Expr<RESULT>> {
  auto &leftExpr{operation.left()};
  leftExpr = Fold(context, std::move(leftExpr));
  auto &rightExpr{operation.right()};
  rightExpr = Fold(context, std::move(rightExpr));
  if (leftExpr.Rank() > 0) {
    if (std::optional<Shape> leftShape{GetShape(context, leftExpr)}) {
      if (auto left{AsFlatArrayConstructor(leftExpr)}) {
        if (rightExpr.Rank() > 0) {
          if (std::optional<Shape> rightShape{GetShape(context, rightExpr)}) {
            if (auto right{AsFlatArrayConstructor(rightExpr)}) {
              CheckConformance(context.messages(), *leftShape, *rightShape);
              return MapOperation(context, std::move(f), *leftShape,
                  std::move(*left), std::move(*right));
            }
          }
        } else if (IsExpandableScalar(rightExpr)) {
          return MapOperation(
              context, std::move(f), *leftShape, std::move(*left), rightExpr);
        }
      }
    }
  } else if (rightExpr.Rank() > 0 && IsExpandableScalar(leftExpr)) {
    if (std::optional<Shape> shape{GetShape(context, rightExpr)}) {
      if (auto right{AsFlatArrayConstructor(rightExpr)}) {
        return MapOperation(
            context, std::move(f), *shape, leftExpr, std::move(*right));
      }
    }
  }
  return std::nullopt;
}

template<typename DERIVED, typename RESULT, typename LEFT, typename RIGHT>
auto ApplyElementwise(
    FoldingContext &context, Operation<DERIVED, RESULT, LEFT, RIGHT> &operation)
    -> std::optional<Expr<RESULT>> {
  return ApplyElementwise(context, operation,
      std::function<Expr<RESULT>(Expr<LEFT> &&, Expr<RIGHT> &&)>{
          [](Expr<LEFT> &&left, Expr<RIGHT> &&right) {
            return Expr<RESULT>{DERIVED{std::move(left), std::move(right)}};
          }});
}

// Unary operations

template<typename TO, typename FROM>
common::IfNoLvalue<std::optional<TO>, FROM> ConvertString(FROM &&s) {
  if constexpr (std::is_same_v<TO, FROM>) {
    return std::make_optional<TO>(std::move(s));
  } else {
    // Fortran character conversion is well defined between distinct kinds
    // only when the actual characters are valid 7-bit ASCII.
    TO str;
    for (auto iter{s.cbegin()}; iter != s.cend(); ++iter) {
      if (static_cast<std::uint64_t>(*iter) > 127) {
        return std::nullopt;
      }
      str.push_back(*iter);
    }
    return std::make_optional<TO>(std::move(str));
  }
}

template<typename TO, TypeCategory FROMCAT>
Expr<TO> FoldOperation(
    FoldingContext &context, Convert<TO, FROMCAT> &&convert) {
  if (auto array{ApplyElementwise(context, convert)}) {
    return *array;
  }
  return std::visit(
      [&](auto &kindExpr) -> Expr<TO> {
        using Operand = ResultType<decltype(kindExpr)>;
        char buffer[64];
        if (auto value{GetScalarConstantValue<Operand>(kindExpr)}) {
          if constexpr (TO::category == TypeCategory::Integer) {
            if constexpr (Operand::category == TypeCategory::Integer) {
              auto converted{Scalar<TO>::ConvertSigned(*value)};
              if (converted.overflow) {
                context.messages().Say(
                    "INTEGER(%d) to INTEGER(%d) conversion overflowed"_en_US,
                    Operand::kind, TO::kind);
              }
              return ScalarConstantToExpr(std::move(converted.value));
            } else if constexpr (Operand::category == TypeCategory::Real) {
              auto converted{value->template ToInteger<Scalar<TO>>()};
              if (converted.flags.test(RealFlag::InvalidArgument)) {
                context.messages().Say(
                    "REAL(%d) to INTEGER(%d) conversion: invalid argument"_en_US,
                    Operand::kind, TO::kind);
              } else if (converted.flags.test(RealFlag::Overflow)) {
                context.messages().Say(
                    "REAL(%d) to INTEGER(%d) conversion overflowed"_en_US,
                    Operand::kind, TO::kind);
              }
              return ScalarConstantToExpr(std::move(converted.value));
            }
          } else if constexpr (TO::category == TypeCategory::Real) {
            if constexpr (Operand::category == TypeCategory::Integer) {
              auto converted{Scalar<TO>::FromInteger(*value)};
              if (!converted.flags.empty()) {
                std::snprintf(buffer, sizeof buffer,
                    "INTEGER(%d) to REAL(%d) conversion", Operand::kind,
                    TO::kind);
                RealFlagWarnings(context, converted.flags, buffer);
              }
              return ScalarConstantToExpr(std::move(converted.value));
            } else if constexpr (Operand::category == TypeCategory::Real) {
              auto converted{Scalar<TO>::Convert(*value)};
              if (!converted.flags.empty()) {
                std::snprintf(buffer, sizeof buffer,
                    "REAL(%d) to REAL(%d) conversion", Operand::kind, TO::kind);
                RealFlagWarnings(context, converted.flags, buffer);
              }
              if (context.flushSubnormalsToZero()) {
                converted.value = converted.value.FlushSubnormalToZero();
              }
              return ScalarConstantToExpr(std::move(converted.value));
            }
          } else if constexpr (TO::category == TypeCategory::Character &&
              Operand::category == TypeCategory::Character) {
            if (auto converted{ConvertString<Scalar<TO>>(std::move(*value))}) {
              return ScalarConstantToExpr(std::move(*converted));
            }
          } else if constexpr (TO::category == TypeCategory::Logical &&
              Operand::category == TypeCategory::Logical) {
            return Expr<TO>{value->IsTrue()};
          }
        }
        return Expr<TO>{std::move(convert)};
      },
      convert.left().u);
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, Parentheses<T> &&x) {
  auto &operand{x.left()};
  operand = Fold(context, std::move(operand));
  if (auto value{GetScalarConstantValue<T>(operand)}) {
    // Preserve parentheses, even around constants.
    return Expr<T>{Parentheses<T>{Expr<T>{Constant<T>{*value}}}};
  }
  return Expr<T>{Parentheses<T>{std::move(operand)}};
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, Negate<T> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  auto &operand{x.left()};
  if (auto value{GetScalarConstantValue<T>(operand)}) {
    if constexpr (T::category == TypeCategory::Integer) {
      auto negated{value->Negate()};
      if (negated.overflow) {
        context.messages().Say(
            "INTEGER(%d) negation overflowed"_en_US, T::kind);
      }
      return Expr<T>{Constant<T>{std::move(negated.value)}};
    } else {
      // REAL & COMPLEX negation: no exceptions possible
      return Expr<T>{Constant<T>{value->Negate()}};
    }
  }
  return Expr<T>{std::move(x)};
}

template<int KIND>
Expr<Type<TypeCategory::Real, KIND>> FoldOperation(
    FoldingContext &context, ComplexComponent<KIND> &&x) {
  using Operand = Type<TypeCategory::Complex, KIND>;
  using Result = Type<TypeCategory::Real, KIND>;
  if (auto array{ApplyElementwise(context, x,
          std::function<Expr<Result>(Expr<Operand> &&)>{
              [=](Expr<Operand> &&operand) {
                return Expr<Result>{ComplexComponent<KIND>{
                    x.isImaginaryPart, std::move(operand)}};
              }})}) {
    return *array;
  }
  using Part = Type<TypeCategory::Real, KIND>;
  auto &operand{x.left()};
  if (auto value{GetScalarConstantValue<Operand>(operand)}) {
    if (x.isImaginaryPart) {
      return Expr<Part>{Constant<Part>{value->AIMAG()}};
    } else {
      return Expr<Part>{Constant<Part>{value->REAL()}};
    }
  }
  return Expr<Part>{std::move(x)};
}

template<int KIND>
Expr<Type<TypeCategory::Logical, KIND>> FoldOperation(
    FoldingContext &context, Not<KIND> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  using Ty = Type<TypeCategory::Logical, KIND>;
  auto &operand{x.left()};
  if (auto value{GetScalarConstantValue<Ty>(operand)}) {
    return Expr<Ty>{Constant<Ty>{!value->IsTrue()}};
  }
  return Expr<Ty>{x};
}

// Binary (dyadic) operations

template<typename LEFT, typename RIGHT>
std::optional<std::pair<Scalar<LEFT>, Scalar<RIGHT>>> OperandsAreConstants(
    const Expr<LEFT> &x, const Expr<RIGHT> &y) {
  if (auto xvalue{GetScalarConstantValue<LEFT>(x)}) {
    if (auto yvalue{GetScalarConstantValue<RIGHT>(y)}) {
      return {std::make_pair(*xvalue, *yvalue)};
    }
  }
  return std::nullopt;
}

template<typename DERIVED, typename RESULT, typename LEFT, typename RIGHT>
std::optional<std::pair<Scalar<LEFT>, Scalar<RIGHT>>> OperandsAreConstants(
    const Operation<DERIVED, RESULT, LEFT, RIGHT> &operation) {
  return OperandsAreConstants(operation.left(), operation.right());
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, Add<T> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  if (auto folded{OperandsAreConstants(x)}) {
    if constexpr (T::category == TypeCategory::Integer) {
      auto sum{folded->first.AddSigned(folded->second)};
      if (sum.overflow) {
        context.messages().Say(
            "INTEGER(%d) addition overflowed"_en_US, T::kind);
      }
      return Expr<T>{Constant<T>{sum.value}};
    } else {
      auto sum{folded->first.Add(folded->second, context.rounding())};
      RealFlagWarnings(context, sum.flags, "addition");
      if (context.flushSubnormalsToZero()) {
        sum.value = sum.value.FlushSubnormalToZero();
      }
      return Expr<T>{Constant<T>{sum.value}};
    }
  }
  return Expr<T>{std::move(x)};
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, Subtract<T> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  if (auto folded{OperandsAreConstants(x)}) {
    if constexpr (T::category == TypeCategory::Integer) {
      auto difference{folded->first.SubtractSigned(folded->second)};
      if (difference.overflow) {
        context.messages().Say(
            "INTEGER(%d) subtraction overflowed"_en_US, T::kind);
      }
      return Expr<T>{Constant<T>{difference.value}};
    } else {
      auto difference{
          folded->first.Subtract(folded->second, context.rounding())};
      RealFlagWarnings(context, difference.flags, "subtraction");
      if (context.flushSubnormalsToZero()) {
        difference.value = difference.value.FlushSubnormalToZero();
      }
      return Expr<T>{Constant<T>{difference.value}};
    }
  }
  return Expr<T>{std::move(x)};
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, Multiply<T> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  if (auto folded{OperandsAreConstants(x)}) {
    if constexpr (T::category == TypeCategory::Integer) {
      auto product{folded->first.MultiplySigned(folded->second)};
      if (product.SignedMultiplicationOverflowed()) {
        context.messages().Say(
            "INTEGER(%d) multiplication overflowed"_en_US, T::kind);
      }
      return Expr<T>{Constant<T>{product.lower}};
    } else {
      auto product{folded->first.Multiply(folded->second, context.rounding())};
      RealFlagWarnings(context, product.flags, "multiplication");
      if (context.flushSubnormalsToZero()) {
        product.value = product.value.FlushSubnormalToZero();
      }
      return Expr<T>{Constant<T>{product.value}};
    }
  }
  return Expr<T>{std::move(x)};
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, Divide<T> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  if (auto folded{OperandsAreConstants(x)}) {
    if constexpr (T::category == TypeCategory::Integer) {
      auto quotAndRem{folded->first.DivideSigned(folded->second)};
      if (quotAndRem.divisionByZero) {
        context.messages().Say("INTEGER(%d) division by zero"_en_US, T::kind);
      }
      if (quotAndRem.overflow) {
        context.messages().Say(
            "INTEGER(%d) division overflowed"_en_US, T::kind);
      }
      return Expr<T>{Constant<T>{quotAndRem.quotient}};
    } else {
      auto quotient{folded->first.Divide(folded->second, context.rounding())};
      RealFlagWarnings(context, quotient.flags, "division");
      if (context.flushSubnormalsToZero()) {
        quotient.value = quotient.value.FlushSubnormalToZero();
      }
      return Expr<T>{Constant<T>{quotient.value}};
    }
  }
  return Expr<T>{std::move(x)};
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, Power<T> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  if (auto folded{OperandsAreConstants(x)}) {
    if constexpr (T::category == TypeCategory::Integer) {
      auto power{folded->first.Power(folded->second)};
      if (power.divisionByZero) {
        context.messages().Say(
            "INTEGER(%d) zero to negative power"_en_US, T::kind);
      } else if (power.overflow) {
        context.messages().Say("INTEGER(%d) power overflowed"_en_US, T::kind);
      } else if (power.zeroToZero) {
        context.messages().Say(
            "INTEGER(%d) 0**0 is not defined"_en_US, T::kind);
      }
      return Expr<T>{Constant<T>{power.power}};
    } else {
      // TODO: real & complex power with non-integral exponent
    }
  }
  return Expr<T>{std::move(x)};
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, RealToIntPower<T> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  return std::visit(
      [&](auto &y) -> Expr<T> {
        if (auto folded{OperandsAreConstants(x.left(), y)}) {
          auto power{evaluate::IntPower(folded->first, folded->second)};
          RealFlagWarnings(context, power.flags, "power with INTEGER exponent");
          if (context.flushSubnormalsToZero()) {
            power.value = power.value.FlushSubnormalToZero();
          }
          return Expr<T>{Constant<T>{power.value}};
        } else {
          return Expr<T>{std::move(x)};
        }
      },
      x.right().u);
}

template<typename T>
Expr<T> FoldOperation(FoldingContext &context, Extremum<T> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  if (auto folded{OperandsAreConstants(x)}) {
    if constexpr (T::category == TypeCategory::Integer) {
      if (folded->first.CompareSigned(folded->second) == x.ordering) {
        return Expr<T>{Constant<T>{folded->first}};
      }
    } else if constexpr (T::category == TypeCategory::Real) {
      if (folded->first.IsNotANumber() ||
          (folded->first.Compare(folded->second) == Relation::Less) ==
              (x.ordering == Ordering::Less)) {
        return Expr<T>{Constant<T>{folded->first}};
      }
    } else {
      if (x.ordering == Compare(folded->first, folded->second)) {
        return Expr<T>{Constant<T>{folded->first}};
      }
    }
    return Expr<T>{Constant<T>{folded->second}};
  }
  return Expr<T>{std::move(x)};
}

template<int KIND>
Expr<Type<TypeCategory::Complex, KIND>> FoldOperation(
    FoldingContext &context, ComplexConstructor<KIND> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  using Result = Type<TypeCategory::Complex, KIND>;
  if (auto folded{OperandsAreConstants(x)}) {
    return Expr<Result>{
        Constant<Result>{Scalar<Result>{folded->first, folded->second}}};
  }
  return Expr<Result>{std::move(x)};
}

template<int KIND>
Expr<Type<TypeCategory::Character, KIND>> FoldOperation(
    FoldingContext &context, Concat<KIND> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  using Result = Type<TypeCategory::Character, KIND>;
  if (auto folded{OperandsAreConstants(x)}) {
    return Expr<Result>{Constant<Result>{folded->first + folded->second}};
  }
  return Expr<Result>{std::move(x)};
}

template<int KIND>
Expr<Type<TypeCategory::Character, KIND>> FoldOperation(
    FoldingContext &context, SetLength<KIND> &&x) {
  if (auto array{ApplyElementwise(context, x)}) {
    return *array;
  }
  using Result = Type<TypeCategory::Character, KIND>;
  if (auto folded{OperandsAreConstants(x)}) {
    auto oldLength{static_cast<std::int64_t>(folded->first.size())};
    auto newLength{folded->second.ToInt64()};
    if (newLength < oldLength) {
      folded->first.erase(newLength);
    } else {
      folded->first.append(newLength - oldLength, ' ');
    }
    CHECK(static_cast<std::int64_t>(folded->first.size()) == newLength);
    return Expr<Result>{Constant<Result>{std::move(folded->first)}};
  }
  return Expr<Result>{std::move(x)};
}

template<typename T>
Expr<LogicalResult> FoldOperation(
    FoldingContext &context, Relational<T> &&relation) {
  if (auto array{ApplyElementwise(context, relation,
          std::function<Expr<LogicalResult>(Expr<T> &&, Expr<T> &&)>{
              [=](Expr<T> &&x, Expr<T> &&y) {
                return Expr<LogicalResult>{Relational<SomeType>{
                    Relational<T>{relation.opr, std::move(x), std::move(y)}}};
              }})}) {
    return *array;
  }
  if (auto folded{OperandsAreConstants(relation)}) {
    bool result{};
    if constexpr (T::category == TypeCategory::Integer) {
      result =
          Satisfies(relation.opr, folded->first.CompareSigned(folded->second));
    } else if constexpr (T::category == TypeCategory::Real) {
      result = Satisfies(relation.opr, folded->first.Compare(folded->second));
    } else if constexpr (T::category == TypeCategory::Character) {
      result = Satisfies(relation.opr, Compare(folded->first, folded->second));
    } else {
      static_assert(T::category != TypeCategory::Complex &&
          T::category != TypeCategory::Logical);
    }
    return Expr<LogicalResult>{Constant<LogicalResult>{result}};
  }
  return Expr<LogicalResult>{Relational<SomeType>{std::move(relation)}};
}

inline Expr<LogicalResult> FoldOperation(
    FoldingContext &context, Relational<SomeType> &&relation) {
  return std::visit(
      [&](auto &&x) {
        return Expr<LogicalResult>{FoldOperation(context, std::move(x))};
      },
      std::move(relation.u));
}

template<int KIND>
Expr<Type<TypeCategory::Logical, KIND>> FoldOperation(
    FoldingContext &context, LogicalOperation<KIND> &&operation) {
  using LOGICAL = Type<TypeCategory::Logical, KIND>;
  if (auto array{ApplyElementwise(context, operation,
          std::function<Expr<LOGICAL>(Expr<LOGICAL> &&, Expr<LOGICAL> &&)>{
              [=](Expr<LOGICAL> &&x, Expr<LOGICAL> &&y) {
                return Expr<LOGICAL>{LogicalOperation<KIND>{
                    operation.logicalOperator, std::move(x), std::move(y)}};
              }})}) {
    return *array;
  }
  if (auto folded{OperandsAreConstants(operation)}) {
    bool xt{folded->first.IsTrue()}, yt{folded->second.IsTrue()}, result{};
    switch (operation.logicalOperator) {
    case LogicalOperator::And: result = xt && yt; break;
    case LogicalOperator::Or: result = xt || yt; break;
    case LogicalOperator::Eqv: result = xt == yt; break;
    case LogicalOperator::Neqv: result = xt != yt; break;
    }
    return Expr<LOGICAL>{Constant<LOGICAL>{result}};
  }
  return Expr<LOGICAL>{std::move(operation)};
}

// end per-operation folding functions

template<typename T>
Expr<T> ExpressionBase<T>::Rewrite(FoldingContext &context, Expr<T> &&expr) {
  return std::visit(
      [&](auto &&x) -> Expr<T> {
        if constexpr (IsSpecificIntrinsicType<T>) {
          return FoldOperation(context, std::move(x));
        } else if constexpr (std::is_same_v<T, SomeDerived>) {
          return FoldOperation(context, std::move(x));
        } else if constexpr (common::HasMember<decltype(x),
                                 TypelessExpression>) {
          return std::move(expr);
        } else {
          return Expr<T>{Fold(context, std::move(x))};
        }
      },
      std::move(expr.u));
}

FOR_EACH_TYPE_AND_KIND(template class ExpressionBase, )

// Constant expression predicate IsConstantExpr().
// This code determines whether an expression is a "constant expression"
// in the sense of section 10.1.12.  This is not the same thing as being
// able to fold it (yet) into a known constant value; specifically,
// the expression may reference derived type kind parameters whose values
// are not yet known.
class IsConstantExprVisitor : public virtual VisitorBase<bool> {
public:
  using Result = bool;
  explicit IsConstantExprVisitor(int) { result() = true; }

  template<int KIND> void Handle(const TypeParamInquiry<KIND> &inq) {
    Check(inq.parameter().template get<semantics::TypeParamDetails>().attr() ==
        common::TypeParamAttr::Kind);
  }
  void Handle(const semantics::Symbol &symbol) {
    Check(symbol.attrs().test(semantics::Attr::PARAMETER));
  }
  void Handle(const CoarrayRef &) { Return(false); }
  void Pre(const semantics::ParamValue &param) { Check(param.isExplicit()); }
  template<typename T> void Pre(const FunctionRef<T> &call) {
    if (const auto *intrinsic{std::get_if<SpecificIntrinsic>(&call.proc().u)}) {
      Check(intrinsic->name == "kind");
      // TODO: Obviously many other intrinsics can be allowed
    } else {
      Return(false);
    }
  }

private:
  void Check(bool ok) {
    if (!ok) {
      Return(false);
    }
  }
};

bool IsConstantExpr(const Expr<SomeType> &expr) {
  return Visitor<IsConstantExprVisitor>{0}.Traverse(expr);
}

std::optional<std::int64_t> ToInt64(const Expr<SomeInteger> &expr) {
  return std::visit(
      [](const auto &kindExpr) { return ToInt64(kindExpr); }, expr.u);
}

std::optional<std::int64_t> ToInt64(const Expr<SomeType> &expr) {
  if (const auto *intExpr{UnwrapExpr<Expr<SomeInteger>>(expr)}) {
    return ToInt64(*intExpr);
  } else {
    return std::nullopt;
  }
}
}
