//#include "vec/functions/function_factory.h"
#include "vec/functions/simple_function_factory.h"
#include "vec/functions/function_binary_arithmetic.h"
#include "vec/common/arithmetic_overflow.h"

namespace doris::vectorized
{

template <typename A, typename B>
struct MultiplyImpl
{
    using ResultType = typename NumberTraits::ResultOfAdditionMultiplication<A, B>::Type;
    static const constexpr bool allow_decimal = true;

    template <typename Result = ResultType>
    static inline NO_SANITIZE_UNDEFINED Result apply(A a, B b)
    {
        return static_cast<Result>(a) * b;
    }

    /// Apply operation and check overflow. It's used for Deciamal operations. @returns true if overflowed, false otherwise.
    template <typename Result = ResultType>
    static inline bool apply(A a, B b, Result & c)
    {
        return common::mulOverflow(static_cast<Result>(a), b, c);
    }

#if USE_EMBEDDED_COMPILER
    static constexpr bool compilable = true;

    static inline llvm::Value * compile(llvm::IRBuilder<> & b, llvm::Value * left, llvm::Value * right, bool)
    {
        return left->getType()->isIntegerTy() ? b.CreateMul(left, right) : b.CreateFMul(left, right);
    }
#endif
};

struct NameMultiply { static constexpr auto name = "multiply"; };
using FunctionMultiply = FunctionBinaryArithmetic<MultiplyImpl, NameMultiply>;

//void registerFunctionMultiply(FunctionFactory & factory)
//{
//    factory.registerFunction<FunctionMultiply>();
//}

void registerFunctionMultiply(SimpleFunctionFactory & factory)
{
    factory.registerFunction<FunctionMultiply>();
}

}
