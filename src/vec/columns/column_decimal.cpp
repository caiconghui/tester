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

#include "vec/common/arena.h"
#include "vec/common/assert_cast.h"
#include "vec/common/exception.h"
#include "vec/common/sip_hash.h"
#include "vec/common/unaligned.h"

//#include <IO/WriteHelpers.h>

#include "vec/columns/column_decimal.h"
#include "vec/columns/columns_common.h"
//#include <DataStreams/ColumnGathererStream.h>

template <typename T>
bool decimalLess(T x, T y, UInt32 x_scale, UInt32 y_scale);

namespace doris::vectorized {

namespace ErrorCodes {
extern const int PARAMETER_OUT_OF_BOUND;
extern const int SIZES_OF_COLUMNS_DOESNT_MATCH;
extern const int NOT_IMPLEMENTED;
} // namespace ErrorCodes

template <typename T>
int ColumnDecimal<T>::compareAt(size_t n, size_t m, const IColumn& rhs_, int) const {
    auto& other = static_cast<const Self&>(rhs_);
    const T& a = data[n];
    const T& b = other.data[m];

    if (scale == other.scale) return a > b ? 1 : (a < b ? -1 : 0);
    return decimalLess<T>(b, a, other.scale, scale)
                   ? 1
                   : (decimalLess<T>(a, b, scale, other.scale) ? -1 : 0);
}

template <typename T>
StringRef ColumnDecimal<T>::serializeValueIntoArena(size_t n, Arena& arena,
                                                    char const*& begin) const {
    auto pos = arena.allocContinue(sizeof(T), begin);
    memcpy(pos, &data[n], sizeof(T));
    return StringRef(pos, sizeof(T));
}

template <typename T>
const char* ColumnDecimal<T>::deserializeAndInsertFromArena(const char* pos) {
    data.push_back(unalignedLoad<T>(pos));
    return pos + sizeof(T);
}

template <typename T>
UInt64 ColumnDecimal<T>::get64(size_t n) const {
    if constexpr (sizeof(T) > sizeof(UInt64))
        throw Exception(String("Method get64 is not supported for ") + getFamilyName(),
                        ErrorCodes::NOT_IMPLEMENTED);
    return static_cast<typename T::NativeType>(data[n]);
}

template <typename T>
void ColumnDecimal<T>::updateHashWithValue(size_t n, SipHash& hash) const {
    hash.update(data[n]);
}

template <typename T>
void ColumnDecimal<T>::getPermutation(bool reverse, size_t limit, int , IColumn::Permutation & res) const
{
#if 1 /// TODO: perf test
    if (data.size() <= std::numeric_limits<UInt32>::max())
    {
        PaddedPODArray<UInt32> tmp_res;
        permutation(reverse, limit, tmp_res);

        res.resize(tmp_res.size());
        for (size_t i = 0; i < tmp_res.size(); ++i)
            res[i] = tmp_res[i];
        return;
    }
#endif

    permutation(reverse, limit, res);
}

template <typename T>
ColumnPtr ColumnDecimal<T>::permute(const IColumn::Permutation& perm, size_t limit) const {
    size_t size = limit ? std::min(data.size(), limit) : data.size();
    if (perm.size() < size)
        throw Exception("Size of permutation is less than required.",
                        ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

    auto res = this->create(size, scale);
    typename Self::Container& res_data = res->getData();

    for (size_t i = 0; i < size; ++i) res_data[i] = data[perm[i]];

    return res;
}

template <typename T>
MutableColumnPtr ColumnDecimal<T>::cloneResized(size_t size) const {
    auto res = this->create(0, scale);

    if (size > 0) {
        auto& new_col = static_cast<Self&>(*res);
        new_col.data.resize(size);

        size_t count = std::min(this->size(), size);
        memcpy(new_col.data.data(), data.data(), count * sizeof(data[0]));

        if (size > count) {
            void* tail = &new_col.data[count];
            memset(tail, 0, (size - count) * sizeof(T));
        }
    }

    return res;
}

template <typename T>
void ColumnDecimal<T>::insertData(const char* src, size_t /*length*/) {
    T tmp;
    memcpy(&tmp, src, sizeof(T));
    data.emplace_back(tmp);
}

template <typename T>
void ColumnDecimal<T>::insertRangeFrom(const IColumn& src, size_t start, size_t length) {
    const ColumnDecimal& src_vec = assert_cast<const ColumnDecimal&>(src);

    if (start + length > src_vec.data.size())
        throw Exception("Parameters start = " + std::to_string(start) +
                                ", length = " + std::to_string(length) +
                                " are out of bound in ColumnDecimal<T>::insertRangeFrom method "
                                "(data.size() = " +
                                std::to_string(src_vec.data.size()) + ").",
                        ErrorCodes::PARAMETER_OUT_OF_BOUND);

    size_t old_size = data.size();
    data.resize(old_size + length);
    memcpy(data.data() + old_size, &src_vec.data[start], length * sizeof(data[0]));
}

template <typename T>
ColumnPtr ColumnDecimal<T>::filter(const IColumn::Filter& filt, ssize_t result_size_hint) const {
    size_t size = data.size();
    if (size != filt.size())
        throw Exception("Size of filter doesn't match size of column.",
                        ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

    auto res = this->create(0, scale);
    Container& res_data = res->getData();

    if (result_size_hint) res_data.reserve(result_size_hint > 0 ? result_size_hint : size);

    const UInt8* filt_pos = filt.data();
    const UInt8* filt_end = filt_pos + size;
    const T* data_pos = data.data();

    while (filt_pos < filt_end) {
        if (*filt_pos) res_data.push_back(*data_pos);

        ++filt_pos;
        ++data_pos;
    }

    return res;
}

//template <typename T>
//ColumnPtr ColumnDecimal<T>::index(const IColumn & indexes, size_t limit) const
//{
//    return selectIndexImpl(*this, indexes, limit);
//}

template <typename T>
ColumnPtr ColumnDecimal<T>::replicate(const IColumn::Offsets& offsets) const {
    size_t size = data.size();
    if (size != offsets.size())
        throw Exception("Size of offsets doesn't match size of column.",
                        ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

    auto res = this->create(0, scale);
    if (0 == size) return res;

    typename Self::Container& res_data = res->getData();
    res_data.reserve(offsets.back());

    IColumn::Offset prev_offset = 0;
    for (size_t i = 0; i < size; ++i) {
        size_t size_to_replicate = offsets[i] - prev_offset;
        prev_offset = offsets[i];

        for (size_t j = 0; j < size_to_replicate; ++j) res_data.push_back(data[i]);
    }

    return res;
}

//template <typename T>
//void ColumnDecimal<T>::gather(ColumnGathererStream & gatherer)
//{
//    gatherer.gather(*this);
//}

template <typename T>
void ColumnDecimal<T>::getExtremes(Field& min, Field& max) const {
    if (data.size() == 0) {
        min = NearestFieldType<T>(0, scale);
        max = NearestFieldType<T>(0, scale);
        return;
    }

    T cur_min = data[0];
    T cur_max = data[0];

    for (const T& x : data) {
        if (x < cur_min)
            cur_min = x;
        else if (x > cur_max)
            cur_max = x;
    }

    min = NearestFieldType<T>(cur_min, scale);
    max = NearestFieldType<T>(cur_max, scale);
}

template class ColumnDecimal<Decimal32>;
template class ColumnDecimal<Decimal64>;
template class ColumnDecimal<Decimal128>;
} // namespace doris::vectorized
