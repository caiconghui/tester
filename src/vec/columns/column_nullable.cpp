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

#include "vec/columns/column_nullable.h"

#include "vec/columns/column_const.h"
#include "vec/common/arena.h"
#include "vec/common/assert_cast.h"
#include "vec/common/nan_utils.h"
#include "vec/common/sip_hash.h"
#include "vec/common/typeid_cast.h"
//#include <DataStreams/ColumnGathererStream.h>

namespace doris::vectorized {

namespace ErrorCodes {
extern const int LOGICAL_ERROR;
extern const int ILLEGAL_COLUMN;
extern const int SIZES_OF_NESTED_COLUMNS_ARE_INCONSISTENT;
} // namespace ErrorCodes

ColumnNullable::ColumnNullable(MutableColumnPtr&& nested_column_, MutableColumnPtr&& null_map_)
        : nested_column(std::move(nested_column_)), null_map(std::move(null_map_)) {
    /// ColumnNullable cannot have constant nested column. But constant argument could be passed. Materialize it.
    nested_column = getNestedColumn().convertToFullColumnIfConst();

    if (!getNestedColumn().canBeInsideNullable())
        throw Exception {getNestedColumn().getName() + " cannot be inside Nullable column",
                         ErrorCodes::ILLEGAL_COLUMN};

    if (isColumnConst(*null_map))
        throw Exception {"ColumnNullable cannot have constant null map",
                         ErrorCodes::ILLEGAL_COLUMN};
}

void ColumnNullable::updateHashWithValue(size_t n, SipHash& hash) const {
    const auto& arr = getNullMapData();
    hash.update(arr[n]);
    if (arr[n] == 0) getNestedColumn().updateHashWithValue(n, hash);
}

MutableColumnPtr ColumnNullable::cloneResized(size_t new_size) const {
    MutableColumnPtr new_nested_col = getNestedColumn().cloneResized(new_size);
    auto new_null_map = ColumnUInt8::create();

    if (new_size > 0) {
        new_null_map->getData().resize(new_size);

        size_t count = std::min(size(), new_size);
        memcpy(new_null_map->getData().data(), getNullMapData().data(),
               count * sizeof(getNullMapData()[0]));

        /// If resizing to bigger one, set all new values to NULLs.
        if (new_size > count) memset(&new_null_map->getData()[count], 1, new_size - count);
    }

    return ColumnNullable::create(std::move(new_nested_col), std::move(new_null_map));
}

Field ColumnNullable::operator[](size_t n) const {
    return isNullAt(n) ? Null() : getNestedColumn()[n];
}

void ColumnNullable::get(size_t n, Field& res) const {
    if (isNullAt(n))
        res = Null();
    else
        getNestedColumn().get(n, res);
}

StringRef ColumnNullable::getDataAt(size_t /*n*/) const {
    throw Exception {"Method getDataAt is not supported for " + getName(),
                     ErrorCodes::NOT_IMPLEMENTED};
}

void ColumnNullable::insertData(const char* pos, size_t length) {
    if (pos == nullptr) {
        getNestedColumn().insertDefault();
        getNullMapData().push_back(1);
    } else {
        getNestedColumn().insertData(pos, length);
        getNullMapData().push_back(0);
    }
}

StringRef ColumnNullable::serializeValueIntoArena(size_t n, Arena& arena,
                                                  char const*& begin) const {
    const auto& arr = getNullMapData();
    static constexpr auto s = sizeof(arr[0]);

    auto pos = arena.allocContinue(s, begin);
    memcpy(pos, &arr[n], s);

    if (arr[n]) return StringRef(pos, s);

    auto nested_ref = getNestedColumn().serializeValueIntoArena(n, arena, begin);

    /// serializeValueIntoArena may reallocate memory. Have to use ptr from nested_ref.data and move it back.
    return StringRef(nested_ref.data - s, nested_ref.size + s);
}

const char* ColumnNullable::deserializeAndInsertFromArena(const char* pos) {
    UInt8 val = *reinterpret_cast<const UInt8*>(pos);
    pos += sizeof(val);

    getNullMapData().push_back(val);

    if (val == 0)
        pos = getNestedColumn().deserializeAndInsertFromArena(pos);
    else
        getNestedColumn().insertDefault();

    return pos;
}

void ColumnNullable::insertRangeFrom(const IColumn& src, size_t start, size_t length) {
    const ColumnNullable& nullable_col = assert_cast<const ColumnNullable&>(src);
    getNullMapColumn().insertRangeFrom(*nullable_col.null_map, start, length);
    getNestedColumn().insertRangeFrom(*nullable_col.nested_column, start, length);
}

void ColumnNullable::insert(const Field& x) {
    if (x.isNull()) {
        getNestedColumn().insertDefault();
        getNullMapData().push_back(1);
    } else {
        getNestedColumn().insert(x);
        getNullMapData().push_back(0);
    }
}

void ColumnNullable::insertFrom(const IColumn& src, size_t n) {
    const ColumnNullable& src_concrete = assert_cast<const ColumnNullable&>(src);
    getNestedColumn().insertFrom(src_concrete.getNestedColumn(), n);
    getNullMapData().push_back(src_concrete.getNullMapData()[n]);
}

void ColumnNullable::insertFromNotNullable(const IColumn& src, size_t n) {
    getNestedColumn().insertFrom(src, n);
    getNullMapData().push_back(0);
}

void ColumnNullable::insertRangeFromNotNullable(const IColumn& src, size_t start, size_t length) {
    getNestedColumn().insertRangeFrom(src, start, length);
    getNullMapData().resize_fill(getNullMapData().size() + length, 0);
}

void ColumnNullable::insertManyFromNotNullable(const IColumn& src, size_t position, size_t length) {
    for (size_t i = 0; i < length; ++i) insertFromNotNullable(src, position);
}

void ColumnNullable::popBack(size_t n) {
    getNestedColumn().popBack(n);
    getNullMapColumn().popBack(n);
}

ColumnPtr ColumnNullable::filter(const Filter& filt, ssize_t result_size_hint) const {
    ColumnPtr filtered_data = getNestedColumn().filter(filt, result_size_hint);
    ColumnPtr filtered_null_map = getNullMapColumn().filter(filt, result_size_hint);
    return ColumnNullable::create(filtered_data, filtered_null_map);
}

ColumnPtr ColumnNullable::permute(const Permutation& perm, size_t limit) const {
    ColumnPtr permuted_data = getNestedColumn().permute(perm, limit);
    ColumnPtr permuted_null_map = getNullMapColumn().permute(perm, limit);
    return ColumnNullable::create(permuted_data, permuted_null_map);
}

//ColumnPtr ColumnNullable::index(const IColumn & indexes, size_t limit) const
//{
//    ColumnPtr indexed_data = getNestedColumn().index(indexes, limit);
//    ColumnPtr indexed_null_map = getNullMapColumn().index(indexes, limit);
//    return ColumnNullable::create(indexed_data, indexed_null_map);
//}

int ColumnNullable::compareAt(size_t n, size_t m, const IColumn& rhs_,
                              int null_direction_hint) const {
    /// NULL values share the properties of NaN values.
    /// Here the last parameter of compareAt is called null_direction_hint
    /// instead of the usual nan_direction_hint and is used to implement
    /// the ordering specified by either NULLS FIRST or NULLS LAST in the
    /// ORDER BY construction.

    const ColumnNullable& nullable_rhs = assert_cast<const ColumnNullable&>(rhs_);

    bool lval_is_null = isNullAt(n);
    bool rval_is_null = nullable_rhs.isNullAt(m);

    if (UNLIKELY(lval_is_null || rval_is_null)) {
        if (lval_is_null && rval_is_null)
            return 0;
        else
            return lval_is_null ? null_direction_hint : -null_direction_hint;
    }

    const IColumn& nested_rhs = nullable_rhs.getNestedColumn();
    return getNestedColumn().compareAt(n, m, nested_rhs, null_direction_hint);
}

void ColumnNullable::getPermutation(bool reverse, size_t limit, int null_direction_hint, Permutation & res) const
{
    /// Cannot pass limit because of unknown amount of NULLs.
    getNestedColumn().getPermutation(reverse, 0, null_direction_hint, res);

    if ((null_direction_hint > 0) != reverse)
    {
        /// Shift all NULL values to the end.

        size_t read_idx = 0;
        size_t write_idx = 0;
        size_t end_idx = res.size();

        if (!limit)
            limit = end_idx;
        else
            limit = std::min(end_idx, limit);

        while (read_idx < limit && !isNullAt(res[read_idx]))
        {
            ++read_idx;
            ++write_idx;
        }

        ++read_idx;

        /// Invariants:
        ///  write_idx < read_idx
        ///  write_idx points to NULL
        ///  read_idx will be incremented to position of next not-NULL
        ///  there are range of NULLs between write_idx and read_idx - 1,
        /// We are moving elements from end to begin of this range,
        ///  so range will "bubble" towards the end.
        /// Relative order of NULL elements could be changed,
        ///  but relative order of non-NULLs is preserved.

        while (read_idx < end_idx && write_idx < limit)
        {
            if (!isNullAt(res[read_idx]))
            {
                std::swap(res[read_idx], res[write_idx]);
                ++write_idx;
            }
            ++read_idx;
        }
    }
    else
    {
        /// Shift all NULL values to the beginning.

        ssize_t read_idx = res.size() - 1;
        ssize_t write_idx = res.size() - 1;

        while (read_idx >= 0 && !isNullAt(res[read_idx]))
        {
            --read_idx;
            --write_idx;
        }

        --read_idx;

        while (read_idx >= 0 && write_idx >= 0)
        {
            if (!isNullAt(res[read_idx]))
            {
                std::swap(res[read_idx], res[write_idx]);
                --write_idx;
            }
            --read_idx;
        }
    }
}
//
//void ColumnNullable::gather(ColumnGathererStream & gatherer)
//{
//    gatherer.gather(*this);
//}

void ColumnNullable::reserve(size_t n) {
    getNestedColumn().reserve(n);
    getNullMapData().reserve(n);
}

size_t ColumnNullable::byteSize() const {
    return getNestedColumn().byteSize() + getNullMapColumn().byteSize();
}

size_t ColumnNullable::allocatedBytes() const {
    return getNestedColumn().allocatedBytes() + getNullMapColumn().allocatedBytes();
}

void ColumnNullable::protect() {
    getNestedColumn().protect();
    getNullMapColumn().protect();
}

namespace {

/// The following function implements a slightly more general version
/// of getExtremes() than the implementation from ColumnVector.
/// It takes into account the possible presence of nullable values.
template <typename T>
void getExtremesFromNullableContent(const ColumnVector<T>& col, const NullMap& null_map, Field& min,
                                    Field& max) {
    const auto& data = col.getData();
    size_t size = data.size();

    if (size == 0) {
        min = Null();
        max = Null();
        return;
    }

    bool has_not_null = false;
    bool has_not_nan = false;

    T cur_min = 0;
    T cur_max = 0;

    for (size_t i = 0; i < size; ++i) {
        const T x = data[i];

        if (null_map[i]) continue;

        if (!has_not_null) {
            cur_min = x;
            cur_max = x;
            has_not_null = true;
            has_not_nan = !isNaN(x);
            continue;
        }

        if (isNaN(x)) continue;

        if (!has_not_nan) {
            cur_min = x;
            cur_max = x;
            has_not_nan = true;
            continue;
        }

        if (x < cur_min)
            cur_min = x;
        else if (x > cur_max)
            cur_max = x;
    }

    if (has_not_null) {
        min = cur_min;
        max = cur_max;
    }
}

} // namespace

void ColumnNullable::getExtremes(Field& min, Field& max) const {
    min = Null();
    max = Null();

    const auto& null_map_data = getNullMapData();

    if (const auto col_i8 = typeid_cast<const ColumnInt8*>(nested_column.get()))
        getExtremesFromNullableContent<Int8>(*col_i8, null_map_data, min, max);
    else if (const auto col_i16 = typeid_cast<const ColumnInt16*>(nested_column.get()))
        getExtremesFromNullableContent<Int16>(*col_i16, null_map_data, min, max);
    else if (const auto col_i32 = typeid_cast<const ColumnInt32*>(nested_column.get()))
        getExtremesFromNullableContent<Int32>(*col_i32, null_map_data, min, max);
    else if (const auto col_i64 = typeid_cast<const ColumnInt64*>(nested_column.get()))
        getExtremesFromNullableContent<Int64>(*col_i64, null_map_data, min, max);
    else if (const auto col_u8 = typeid_cast<const ColumnUInt8*>(nested_column.get()))
        getExtremesFromNullableContent<UInt8>(*col_u8, null_map_data, min, max);
    else if (const auto col_u16 = typeid_cast<const ColumnUInt16*>(nested_column.get()))
        getExtremesFromNullableContent<UInt16>(*col_u16, null_map_data, min, max);
    else if (const auto col_u32 = typeid_cast<const ColumnUInt32*>(nested_column.get()))
        getExtremesFromNullableContent<UInt32>(*col_u32, null_map_data, min, max);
    else if (const auto col_u64 = typeid_cast<const ColumnUInt64*>(nested_column.get()))
        getExtremesFromNullableContent<UInt64>(*col_u64, null_map_data, min, max);
    else if (const auto col_f32 = typeid_cast<const ColumnFloat32*>(nested_column.get()))
        getExtremesFromNullableContent<Float32>(*col_f32, null_map_data, min, max);
    else if (const auto col_f64 = typeid_cast<const ColumnFloat64*>(nested_column.get()))
        getExtremesFromNullableContent<Float64>(*col_f64, null_map_data, min, max);
}

ColumnPtr ColumnNullable::replicate(const Offsets& offsets) const {
    ColumnPtr replicated_data = getNestedColumn().replicate(offsets);
    ColumnPtr replicated_null_map = getNullMapColumn().replicate(offsets);
    return ColumnNullable::create(replicated_data, replicated_null_map);
}

template <bool negative>
void ColumnNullable::applyNullMapImpl(const ColumnUInt8& map) {
    NullMap& arr1 = getNullMapData();
    const NullMap& arr2 = map.getData();

    if (arr1.size() != arr2.size())
        throw Exception {"Inconsistent sizes of ColumnNullable objects", ErrorCodes::LOGICAL_ERROR};

    for (size_t i = 0, size = arr1.size(); i < size; ++i) arr1[i] |= negative ^ arr2[i];
}

void ColumnNullable::applyNullMap(const ColumnUInt8& map) {
    applyNullMapImpl<false>(map);
}

void ColumnNullable::applyNegatedNullMap(const ColumnUInt8& map) {
    applyNullMapImpl<true>(map);
}

void ColumnNullable::applyNullMap(const ColumnNullable& other) {
    applyNullMap(other.getNullMapColumn());
}

void ColumnNullable::checkConsistency() const {
    if (null_map->size() != getNestedColumn().size())
        throw Exception(
                "Logical error: Sizes of nested column and null map of Nullable column are not "
                "equal",
                ErrorCodes::SIZES_OF_NESTED_COLUMNS_ARE_INCONSISTENT);
}

ColumnPtr makeNullable(const ColumnPtr& column) {
    if (isColumnNullable(*column)) return column;

    if (isColumnConst(*column))
        return ColumnConst::create(
                makeNullable(assert_cast<const ColumnConst&>(*column).getDataColumnPtr()),
                column->size());

    return ColumnNullable::create(column, ColumnUInt8::create(column->size(), 0));
}

} // namespace doris::vectorized
