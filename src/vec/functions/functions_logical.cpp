// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

//#include <vec/Functions/FunctionFactory.h>
#include "vec/functions/functions_logical.h"

#include <algorithm>

#include "vec/columns/column.h"
#include "vec/columns/column_const.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_vector.h"
#include "vec/columns/columns_number.h"
#include "vec/common/field_visitors.h"
#include "vec/common/typeid_cast.h"
#include "vec/data_types/data_type_nullable.h"
#include "vec/data_types/data_types_number.h"
#include "vec/functions/function_helpers.h"
#include "vec/functions/simple_function_factory.h"

namespace doris::vectorized {

//void registerFunctionsLogical(FunctionFactory & factory)
//{
//    factory.registerFunction<FunctionAnd>();
//    factory.registerFunction<FunctionOr>();
//    factory.registerFunction<FunctionXor>();
//    factory.registerFunction<FunctionNot>();
//}

namespace ErrorCodes {
extern const int LOGICAL_ERROR;
extern const int ILLEGAL_TYPE_OF_ARGUMENT;
extern const int TOO_FEW_ARGUMENTS_FOR_FUNCTION;
extern const int ILLEGAL_COLUMN;
} // namespace ErrorCodes

namespace {
using namespace FunctionsLogicalDetail;

using UInt8Container = ColumnUInt8::Container;
using UInt8ColumnPtrs = std::vector<const ColumnUInt8*>;

MutableColumnPtr convertFromTernaryData(const UInt8Container& ternary_data,
                                        const bool make_nullable) {
    const size_t rows_count = ternary_data.size();

    auto new_column = ColumnUInt8::create(rows_count);
    std::transform(ternary_data.cbegin(), ternary_data.cend(), new_column->getData().begin(),
                   [](const auto x) { return x == Ternary::True; });

    if (!make_nullable) return new_column;

    auto null_column = ColumnUInt8::create(rows_count);
    std::transform(ternary_data.cbegin(), ternary_data.cend(), null_column->getData().begin(),
                   [](const auto x) { return x == Ternary::Null; });

    return ColumnNullable::create(std::move(new_column), std::move(null_column));
}

template <typename T>
bool tryConvertColumnToUInt8(const IColumn* column, UInt8Container& res) {
    const auto col = checkAndGetColumn<ColumnVector<T>>(column);
    if (!col) return false;

    std::transform(col->getData().cbegin(), col->getData().cend(), res.begin(),
                   [](const auto x) { return x != 0; });

    return true;
}

void convertColumnToUInt8(const IColumn* column, UInt8Container& res) {
    if (!tryConvertColumnToUInt8<Int8>(column, res) &&
        !tryConvertColumnToUInt8<Int16>(column, res) &&
        !tryConvertColumnToUInt8<Int32>(column, res) &&
        !tryConvertColumnToUInt8<Int64>(column, res) &&
        !tryConvertColumnToUInt8<UInt16>(column, res) &&
        !tryConvertColumnToUInt8<UInt32>(column, res) &&
        !tryConvertColumnToUInt8<UInt64>(column, res) &&
        !tryConvertColumnToUInt8<Float32>(column, res) &&
        !tryConvertColumnToUInt8<Float64>(column, res))
        throw Exception("Unexpected type of column: " + column->getName(),
                        ErrorCodes::ILLEGAL_COLUMN);
}

template <class Op, typename Func>
static bool extractConstColumns(ColumnRawPtrs& in, UInt8& res, Func&& func) {
    bool has_res = false;

    for (int i = static_cast<int>(in.size()) - 1; i >= 0; --i) {
        if (!isColumnConst(*in[i])) continue;

        UInt8 x = func((*in[i])[0]);
        if (has_res) {
            res = Op::apply(res, x);
        } else {
            res = x;
            has_res = true;
        }

        in.erase(in.begin() + i);
    }

    return has_res;
}

template <class Op>
inline bool extractConstColumns(ColumnRawPtrs& in, UInt8& res) {
    return extractConstColumns<Op>(in, res, [](const Field& value) {
        return !value.isNull() && applyVisitor(FieldVisitorConvertToNumber<bool>(), value);
    });
}

template <class Op>
inline bool extractConstColumnsTernary(ColumnRawPtrs& in, UInt8& res_3v) {
    return extractConstColumns<Op>(in, res_3v, [](const Field& value) {
        return value.isNull() ? Ternary::makeValue(false, true)
                              : Ternary::makeValue(
                                        applyVisitor(FieldVisitorConvertToNumber<bool>(), value));
    });
}

template <typename Op, size_t N>
class AssociativeApplierImpl {
    using ResultValueType = typename Op::ResultType;

public:
    /// Remembers the last N columns from `in`.
    AssociativeApplierImpl(const UInt8ColumnPtrs& in)
            : vec(in[in.size() - N]->getData()), next(in) {}

    /// Returns a combination of values in the i-th row of all columns stored in the constructor.
    inline ResultValueType apply(const size_t i) const {
        const auto& a = vec[i];
        if constexpr (Op::isSaturable())
            return Op::isSaturatedValue(a) ? a : Op::apply(a, next.apply(i));
        else
            return Op::apply(a, next.apply(i));
    }

private:
    const UInt8Container& vec;
    const AssociativeApplierImpl<Op, N - 1> next;
};

template <typename Op>
class AssociativeApplierImpl<Op, 1> {
    using ResultValueType = typename Op::ResultType;

public:
    AssociativeApplierImpl(const UInt8ColumnPtrs& in) : vec(in[in.size() - 1]->getData()) {}

    inline ResultValueType apply(const size_t i) const { return vec[i]; }

private:
    const UInt8Container& vec;
};

/// A helper class used by AssociativeGenericApplierImpl
/// Allows for on-the-fly conversion of any data type into intermediate ternary representation
using ValueGetter = std::function<Ternary::ResultType(size_t)>;

template <typename... Types>
struct ValueGetterBuilderImpl;

template <typename Type, typename... Types>
struct ValueGetterBuilderImpl<Type, Types...> {
    static ValueGetter build(const IColumn* x) {
        if (const auto nullable_column = typeid_cast<const ColumnNullable*>(x)) {
            if (const auto nested_column = typeid_cast<const ColumnVector<Type>*>(
                        nullable_column->getNestedColumnPtr().get())) {
                return [&null_data = nullable_column->getNullMapData(),
                        &column_data = nested_column->getData()](size_t i) {
                    return Ternary::makeValue(column_data[i], null_data[i]);
                };
            } else
                return ValueGetterBuilderImpl<Types...>::build(x);
        } else if (const auto column = typeid_cast<const ColumnVector<Type>*>(x))
            return [&column_data = column->getData()](size_t i) {
                return Ternary::makeValue(column_data[i]);
            };
        else
            return ValueGetterBuilderImpl<Types...>::build(x);
    }
};

template <>
struct ValueGetterBuilderImpl<> {
    static ValueGetter build(const IColumn* x) {
        throw Exception(
                std::string("Unknown numeric column of type: ") + demangle(typeid(x).name()),
                ErrorCodes::LOGICAL_ERROR);
    }
};

using ValueGetterBuilder = ValueGetterBuilderImpl<UInt8, UInt16, UInt32, UInt64, Int8, Int16, Int32,
                                                  Int64, Float32, Float64>;

/// This class together with helper class ValueGetterBuilder can be used with columns of arbitrary data type
/// Allows for on-the-fly conversion of any type of data into intermediate ternary representation
/// and eliminates the need to materialize data columns in intermediate representation
template <typename Op, size_t N>
class AssociativeGenericApplierImpl {
    using ResultValueType = typename Op::ResultType;

public:
    /// Remembers the last N columns from `in`.
    AssociativeGenericApplierImpl(const ColumnRawPtrs& in)
            : val_getter {ValueGetterBuilder::build(in[in.size() - N])}, next {in} {}

    /// Returns a combination of values in the i-th row of all columns stored in the constructor.
    inline ResultValueType apply(const size_t i) const {
        const auto a = val_getter(i);
        if constexpr (Op::isSaturable())
            return Op::isSaturatedValue(a) ? a : Op::apply(a, next.apply(i));
        else
            return Op::apply(a, next.apply(i));
    }

private:
    const ValueGetter val_getter;
    const AssociativeGenericApplierImpl<Op, N - 1> next;
};

template <typename Op>
class AssociativeGenericApplierImpl<Op, 1> {
    using ResultValueType = typename Op::ResultType;

public:
    /// Remembers the last N columns from `in`.
    AssociativeGenericApplierImpl(const ColumnRawPtrs& in)
            : val_getter {ValueGetterBuilder::build(in[in.size() - 1])} {}

    inline ResultValueType apply(const size_t i) const { return val_getter(i); }

private:
    const ValueGetter val_getter;
};

/// Apply target function by feeding it "batches" of N columns
/// Combining 10 columns per pass is the fastest for large block sizes.
/// For small block sizes - more columns is faster.
template <typename Op, template <typename, size_t> typename OperationApplierImpl, size_t N = 10>
struct OperationApplier {
    template <typename Columns, typename ResultColumn>
    static void apply(Columns& in, ResultColumn& result) {
        while (in.size() > 1) {
            doBatchedApply(in, result->getData());
            in.push_back(result.get());
        }
    }

    template <typename Columns, typename ResultData>
    static void NO_INLINE doBatchedApply(Columns& in, ResultData& result_data) {
        if (N > in.size()) {
            OperationApplier<Op, OperationApplierImpl, N - 1>::doBatchedApply(in, result_data);
            return;
        }

        const OperationApplierImpl<Op, N> operationApplierImpl(in);
        size_t i = 0;
        for (auto& res : result_data) res = operationApplierImpl.apply(i++);

        in.erase(in.end() - N, in.end());
    }
};

template <typename Op, template <typename, size_t> typename OperationApplierImpl>
struct OperationApplier<Op, OperationApplierImpl, 1> {
    template <typename Columns, typename Result>
    static void NO_INLINE doBatchedApply(Columns&, Result&) {
        throw Exception(
                "OperationApplier<...>::apply(...): not enough arguments to run this method",
                ErrorCodes::LOGICAL_ERROR);
    }
};

template <class Op>
static void executeForTernaryLogicImpl(ColumnRawPtrs arguments, ColumnWithTypeAndName& result_info,
                                       size_t input_rows_count) {
    /// Combine all constant columns into a single constant value.
    UInt8 const_3v_value = 0;
    const bool has_consts = extractConstColumnsTernary<Op>(arguments, const_3v_value);

    /// If the constant value uniquely determines the result, return it.
    if (has_consts &&
        (arguments.empty() || (Op::isSaturable() && Op::isSaturatedValue(const_3v_value)))) {
        result_info.column =
                ColumnConst::create(convertFromTernaryData(UInt8Container({const_3v_value}),
                                                           result_info.type->isNullable()),
                                    input_rows_count);
        return;
    }

    const auto result_column = ColumnUInt8::create(input_rows_count);
    MutableColumnPtr const_column_holder;
    if (has_consts) {
        const_column_holder = convertFromTernaryData(
                UInt8Container(input_rows_count, const_3v_value), const_3v_value == Ternary::Null);
        arguments.push_back(const_column_holder.get());
    }

    OperationApplier<Op, AssociativeGenericApplierImpl>::apply(arguments, result_column);

    result_info.column =
            convertFromTernaryData(result_column->getData(), result_info.type->isNullable());
}

template <typename Op, typename... Types>
struct TypedExecutorInvoker;

template <typename Op>
using FastApplierImpl = TypedExecutorInvoker<Op, UInt8, UInt16, UInt32, UInt64, Int8, Int16, Int32,
                                             Int64, Float32, Float64>;

template <typename Op, typename Type, typename... Types>
struct TypedExecutorInvoker<Op, Type, Types...> {
    template <typename T, typename Result>
    static void apply(const ColumnVector<T>& x, const IColumn& y, Result& result) {
        if (const auto column = typeid_cast<const ColumnVector<Type>*>(&y))
            std::transform(x.getData().cbegin(), x.getData().cend(), column->getData().cbegin(),
                           result.begin(),
                           [](const auto a, const auto b) { return Op::apply(!!a, !!b); });
        else
            TypedExecutorInvoker<Op, Types...>::template apply<T>(x, y, result);
    }

    template <typename Result>
    static void apply(const IColumn& x, const IColumn& y, Result& result) {
        if (const auto column = typeid_cast<const ColumnVector<Type>*>(&x))
            FastApplierImpl<Op>::template apply<Type>(*column, y, result);
        else
            TypedExecutorInvoker<Op, Types...>::apply(x, y, result);
    }
};

template <typename Op>
struct TypedExecutorInvoker<Op> {
    template <typename T, typename Result>
    static void apply(const ColumnVector<T>&, const IColumn& y, Result&) {
        throw Exception(
                std::string("Unknown numeric column y of type: ") + demangle(typeid(y).name()),
                ErrorCodes::LOGICAL_ERROR);
    }

    template <typename Result>
    static void apply(const IColumn& x, const IColumn&, Result&) {
        throw Exception(
                std::string("Unknown numeric column x of type: ") + demangle(typeid(x).name()),
                ErrorCodes::LOGICAL_ERROR);
    }
};

template <class Op>
static void basicExecuteImpl(ColumnRawPtrs arguments, ColumnWithTypeAndName& result_info,
                             size_t input_rows_count) {
    /// Combine all constant columns into a single constant value.
    UInt8 const_val = 0;
    bool has_consts = extractConstColumns<Op>(arguments, const_val);

    /// If the constant value uniquely determines the result, return it.
    if (has_consts && (arguments.empty() || Op::apply(const_val, 0) == Op::apply(const_val, 1))) {
        if (!arguments.empty()) const_val = Op::apply(const_val, 0);
        result_info.column =
                DataTypeUInt8().createColumnConst(input_rows_count, toField(const_val));
        return;
    }

    /// If the constant value is a neutral element, let's forget about it.
    if (has_consts && Op::apply(const_val, 0) == 0 && Op::apply(const_val, 1) == 1)
        has_consts = false;

    UInt8ColumnPtrs uint8_args;

    auto col_res = ColumnUInt8::create();
    UInt8Container& vec_res = col_res->getData();
    if (has_consts) {
        vec_res.assign(input_rows_count, const_val);
        uint8_args.push_back(col_res.get());
    } else {
        vec_res.resize(input_rows_count);
    }

    /// FastPath detection goes in here
    if (arguments.size() == (has_consts ? 1 : 2)) {
        if (has_consts)
            FastApplierImpl<Op>::apply(*arguments[0], *col_res, col_res->getData());
        else
            FastApplierImpl<Op>::apply(*arguments[0], *arguments[1], col_res->getData());

        result_info.column = std::move(col_res);
        return;
    }

    /// Convert all columns to UInt8
    Columns converted_columns;
    for (const IColumn* column : arguments) {
        if (auto uint8_column = checkAndGetColumn<ColumnUInt8>(column))
            uint8_args.push_back(uint8_column);
        else {
            auto converted_column = ColumnUInt8::create(input_rows_count);
            convertColumnToUInt8(column, converted_column->getData());
            uint8_args.push_back(converted_column.get());
            converted_columns.emplace_back(std::move(converted_column));
        }
    }

    OperationApplier<Op, AssociativeApplierImpl>::apply(uint8_args, col_res);

    /// This is possible if there is exactly one non-constant among the arguments, and it is of type UInt8.
    if (uint8_args[0] != col_res.get()) vec_res.assign(uint8_args[0]->getData());

    result_info.column = std::move(col_res);
}

} // namespace

template <typename Impl, typename Name>
DataTypePtr FunctionAnyArityLogical<Impl, Name>::getReturnTypeImpl(
        const DataTypes& arguments) const {
    if (arguments.size() < 2)
        throw Exception("Number of arguments for function \"" + getName() +
                                "\" should be at least 2: passed " +
                                std::to_string(arguments.size()),
                        ErrorCodes::TOO_FEW_ARGUMENTS_FOR_FUNCTION);

    bool has_nullable_arguments = false;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg_type = arguments[i];

        if (!has_nullable_arguments) {
            has_nullable_arguments = arg_type->isNullable();
            if (has_nullable_arguments && !Impl::specialImplementationForNulls())
                throw Exception(
                        "Logical error: Unexpected type of argument for function \"" + getName() +
                                "\": "
                                " argument " +
                                std::to_string(i + 1) + " is of type " + arg_type->getName(),
                        ErrorCodes::LOGICAL_ERROR);
        }

        if (!(isNativeNumber(arg_type) ||
              (Impl::specialImplementationForNulls() &&
               (arg_type->onlyNull() || isNativeNumber(removeNullable(arg_type))))))
            throw Exception("Illegal type (" + arg_type->getName() + ") of " +
                                    std::to_string(i + 1) + " argument of function " + getName(),
                            ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
    }

    auto result_type = std::make_shared<DataTypeUInt8>();
    return has_nullable_arguments ? makeNullable(result_type) : result_type;
}

template <typename Impl, typename Name>
void FunctionAnyArityLogical<Impl, Name>::executeImpl(Block& block, const ColumnNumbers& arguments,
                                                      size_t result_index,
                                                      size_t input_rows_count) {
    ColumnRawPtrs args_in;
    for (const auto arg_index : arguments)
        args_in.push_back(block.getByPosition(arg_index).column.get());

    auto& result_info = block.getByPosition(result_index);
    if (result_info.type->isNullable())
        executeForTernaryLogicImpl<Impl>(std::move(args_in), result_info, input_rows_count);
    else
        basicExecuteImpl<Impl>(std::move(args_in), result_info, input_rows_count);
}

template <typename A, typename Op>
struct UnaryOperationImpl {
    using ResultType = typename Op::ResultType;
    using ArrayA = typename ColumnVector<A>::Container;
    using ArrayC = typename ColumnVector<ResultType>::Container;

    static void NO_INLINE vector(const ArrayA& a, ArrayC& c) {
        std::transform(a.cbegin(), a.cend(), c.begin(), [](const auto x) { return Op::apply(x); });
    }
};

template <template <typename> class Impl, typename Name>
DataTypePtr FunctionUnaryLogical<Impl, Name>::getReturnTypeImpl(const DataTypes& arguments) const {
    if (!isNativeNumber(arguments[0]))
        throw Exception("Illegal type (" + arguments[0]->getName() + ") of argument of function " +
                                getName(),
                        ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

    return std::make_shared<DataTypeUInt8>();
}

template <template <typename> class Impl, typename T>
bool functionUnaryExecuteType(Block& block, const ColumnNumbers& arguments, size_t result) {
    if (auto col = checkAndGetColumn<ColumnVector<T>>(
                block.getByPosition(arguments[0]).column.get())) {
        auto col_res = ColumnUInt8::create();

        typename ColumnUInt8::Container& vec_res = col_res->getData();
        vec_res.resize(col->getData().size());
        UnaryOperationImpl<T, Impl<T>>::vector(col->getData(), vec_res);

        block.getByPosition(result).column = std::move(col_res);
        return true;
    }

    return false;
}

template <template <typename> class Impl, typename Name>
void FunctionUnaryLogical<Impl, Name>::executeImpl(Block& block, const ColumnNumbers& arguments,
                                                   size_t result, size_t /*input_rows_count*/) {
    if (!(functionUnaryExecuteType<Impl, UInt8>(block, arguments, result) ||
          functionUnaryExecuteType<Impl, UInt16>(block, arguments, result) ||
          functionUnaryExecuteType<Impl, UInt32>(block, arguments, result) ||
          functionUnaryExecuteType<Impl, UInt64>(block, arguments, result) ||
          functionUnaryExecuteType<Impl, Int8>(block, arguments, result) ||
          functionUnaryExecuteType<Impl, Int16>(block, arguments, result) ||
          functionUnaryExecuteType<Impl, Int32>(block, arguments, result) ||
          functionUnaryExecuteType<Impl, Int64>(block, arguments, result) ||
          functionUnaryExecuteType<Impl, Float32>(block, arguments, result) ||
          functionUnaryExecuteType<Impl, Float64>(block, arguments, result)))
        throw Exception("Illegal column " + block.getByPosition(arguments[0]).column->getName() +
                                " of argument of function " + getName(),
                        ErrorCodes::ILLEGAL_COLUMN);
}

void registerFunctionLogical(SimpleFunctionFactory& instance) {
    instance.registerFunction<FunctionAnd>();
    instance.registerFunction<FunctionOr>();
    instance.registerFunction<FunctionXor>();
    instance.registerFunction<FunctionNot>();
}

} // namespace doris::vectorized
