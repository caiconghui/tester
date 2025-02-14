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

#pragma once

#include <cmath>

#include "vec/columns/column.h"
#include "vec/columns/column_impl.h"
#include "vec/columns/column_vector_helper.h"
#include "vec/common/unaligned.h"
#include "vec/core/field.h"

namespace doris::vectorized {

/** Stuff for comparing numbers.
  * Integer values are compared as usual.
  * Floating-point numbers are compared this way that NaNs always end up at the end
  *  (if you don't do this, the sort would not work at all).
  */
template <typename T>
struct CompareHelper {
    static bool less(T a, T b, int /*nan_direction_hint*/) { return a < b; }
    static bool greater(T a, T b, int /*nan_direction_hint*/) { return a > b; }

    /** Compares two numbers. Returns a number less than zero, equal to zero, or greater than zero if a < b, a == b, a > b, respectively.
      * If one of the values is NaN, then
      * - if nan_direction_hint == -1 - NaN are considered less than all numbers;
      * - if nan_direction_hint == 1 - NaN are considered to be larger than all numbers;
      * Essentially: nan_direction_hint == -1 says that the comparison is for sorting in descending order.
      */
    static int compare(T a, T b, int /*nan_direction_hint*/) {
        return a > b ? 1 : (a < b ? -1 : 0);
    }
};

template <typename T>
struct FloatCompareHelper {
    static bool less(T a, T b, int nan_direction_hint) {
        bool isnan_a = std::isnan(a);
        bool isnan_b = std::isnan(b);

        if (isnan_a && isnan_b) return false;
        if (isnan_a) return nan_direction_hint < 0;
        if (isnan_b) return nan_direction_hint > 0;

        return a < b;
    }

    static bool greater(T a, T b, int nan_direction_hint) {
        bool isnan_a = std::isnan(a);
        bool isnan_b = std::isnan(b);

        if (isnan_a && isnan_b) return false;
        if (isnan_a) return nan_direction_hint > 0;
        if (isnan_b) return nan_direction_hint < 0;

        return a > b;
    }

    static int compare(T a, T b, int nan_direction_hint) {
        bool isnan_a = std::isnan(a);
        bool isnan_b = std::isnan(b);
        if (UNLIKELY(isnan_a || isnan_b)) {
            if (isnan_a && isnan_b) return 0;

            return isnan_a ? nan_direction_hint : -nan_direction_hint;
        }

        return (T(0) < (a - b)) - ((a - b) < T(0));
    }
};

template <>
struct CompareHelper<Float32> : public FloatCompareHelper<Float32> {};
template <>
struct CompareHelper<Float64> : public FloatCompareHelper<Float64> {};

/** A template for columns that use a simple array to store.
 */
template <typename T>
class ColumnVector final : public COWHelper<ColumnVectorHelper, ColumnVector<T>> {
    static_assert(!IsDecimalNumber<T>);

private:
    using Self = ColumnVector;
    friend class COWHelper<ColumnVectorHelper, Self>;

    struct less;
    struct greater;

public:
    using value_type = T;
    using Container = PaddedPODArray<value_type>;

private:
    ColumnVector() {}
    ColumnVector(const size_t n) : data(n) {}
    ColumnVector(const size_t n, const value_type x) : data(n, x) {}
    ColumnVector(const ColumnVector& src) : data(src.data.begin(), src.data.end()) {}

    /// Sugar constructor.
    ColumnVector(std::initializer_list<T> il) : data {il} {}

public:
    bool isNumeric() const override { return IsNumber<T>; }

    size_t size() const override { return data.size(); }

    StringRef getDataAt(size_t n) const override {
        return StringRef(reinterpret_cast<const char*>(&data[n]), sizeof(data[n]));
    }

    void insertFrom(const IColumn& src, size_t n) override {
        data.push_back(static_cast<const Self&>(src).getData()[n]);
    }

    void insertData(const char* pos, size_t /*length*/) override {
        data.push_back(unalignedLoad<T>(pos));
    }

    void insertDefault() override { data.push_back(T()); }

    void popBack(size_t n) override { data.resize_assume_reserved(data.size() - n); }

    StringRef serializeValueIntoArena(size_t n, Arena& arena, char const*& begin) const override;

    const char* deserializeAndInsertFromArena(const char* pos) override;

    void updateHashWithValue(size_t n, SipHash& hash) const override;

    size_t byteSize() const override { return data.size() * sizeof(data[0]); }

    size_t allocatedBytes() const override { return data.allocated_bytes(); }

    void protect() override { data.protect(); }

    void insertValue(const T value) { data.push_back(value); }

    /// This method implemented in header because it could be possibly devirtualized.
    int compareAt(size_t n, size_t m, const IColumn& rhs_, int nan_direction_hint) const override {
        return CompareHelper<T>::compare(data[n], static_cast<const Self&>(rhs_).data[m],
                                         nan_direction_hint);
    }

    void getPermutation(bool reverse, size_t limit, int nan_direction_hint, IColumn::Permutation & res) const override;

    void reserve(size_t n) override { data.reserve(n); }

    const char* getFamilyName() const override;

    MutableColumnPtr cloneResized(size_t size) const override;

    Field operator[](size_t n) const override { return data[n]; }

    void get(size_t n, Field& res) const override { res = (*this)[n]; }

    UInt64 get64(size_t n) const override;

    Float64 getFloat64(size_t n) const override;

    UInt64 getUInt(size_t n) const override { return UInt64(data[n]); }

    bool getBool(size_t n) const override { return bool(data[n]); }

    Int64 getInt(size_t n) const override { return Int64(data[n]); }

    void insert(const Field& x) override {
        data.push_back(doris::vectorized::get<NearestFieldType<T>>(x));
    }

    void insertRangeFrom(const IColumn& src, size_t start, size_t length) override;

    ColumnPtr filter(const IColumn::Filter& filt, ssize_t result_size_hint) const override;

    ColumnPtr permute(const IColumn::Permutation& perm, size_t limit) const override;

    //    ColumnPtr index(const IColumn & indexes, size_t limit) const override;

    template <typename Type>
    ColumnPtr indexImpl(const PaddedPODArray<Type>& indexes, size_t limit) const;

    ColumnPtr replicate(const IColumn::Offsets& offsets) const override;

    void getExtremes(Field& min, Field& max) const override;

    MutableColumns scatter(IColumn::ColumnIndex num_columns,
                           const IColumn::Selector& selector) const override {
        return this->template scatterImpl<Self>(num_columns, selector);
    }

    //    void gather(ColumnGathererStream & gatherer_stream) override;

    bool canBeInsideNullable() const override { return true; }

    bool isFixedAndContiguous() const override { return true; }
    size_t sizeOfValueIfFixed() const override { return sizeof(T); }
    StringRef getRawData() const override {
        return StringRef(reinterpret_cast<const char*>(data.data()), data.size());
    }

    bool structureEquals(const IColumn& rhs) const override {
        return typeid(rhs) == typeid(ColumnVector<T>);
    }

    /** More efficient methods of manipulation - to manipulate with data directly. */
    Container& getData() { return data; }

    const Container& getData() const { return data; }

    const T& getElement(size_t n) const { return data[n]; }

    T& getElement(size_t n) { return data[n]; }

protected:
    Container data;
};

template <typename T>
template <typename Type>
ColumnPtr ColumnVector<T>::indexImpl(const PaddedPODArray<Type>& indexes, size_t limit) const {
    size_t size = indexes.size();

    if (limit == 0)
        limit = size;
    else
        limit = std::min(size, limit);

    auto res = this->create(limit);
    typename Self::Container& res_data = res->getData();
    for (size_t i = 0; i < limit; ++i) res_data[i] = data[indexes[i]];

    return res;
}

} // namespace doris::vectorized
