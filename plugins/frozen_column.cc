/** frozen_column.cc                                               -*- C++ -*-
    Jeremy Barnes, 27 March 2016
    This file is part of MLDB. Copyright 2016 Datacratic. All rights reserved.

    Implementation of code to freeze columns into a binary format.
*/

#include "frozen_column.h"
#include "tabular_dataset_column.h"
#include "mldb/arch/bitops.h"
#include "mldb/arch/bit_range_ops.h"
#include "mldb/utils/compact_vector.h"
#include "mldb/jml/utils/lightweight_hash.h"
#include "mldb/http/http_exception.h"
#include <mutex>

using namespace std;


namespace MLDB {


/*****************************************************************************/
/* FROZEN COLUMN                                                             */
/*****************************************************************************/


/// Frozen column that finds each value in a lookup table
struct TableFrozenColumn: public FrozenColumn {
    TableFrozenColumn(TabularDatasetColumn & column)
        : table(std::move(column.indexedVals)),
          columnTypes(column.columnTypes)
    {
        firstEntry = column.minRowNumber;
        numEntries = column.maxRowNumber - column.minRowNumber + 1;
        hasNulls = column.sparseIndexes.size() < numEntries;
        indexBits = ML::highest_bit(table.size() + hasNulls) + 1;
        size_t numWords = (indexBits * numEntries + 31) / 32;
        uint32_t * data = new uint32_t[numWords];
        storage = std::shared_ptr<uint32_t>(data, [] (uint32_t * p) { delete[] p; });

        if (!hasNulls) {
            // Contiguous rows
            ML::Bit_Writer<uint32_t> writer(data);
            for (size_t i = 0;  i < column.sparseIndexes.size();  ++i) {
                ExcAssertEqual(column.sparseIndexes[i].first, i);
                writer.write(column.sparseIndexes[i].second, indexBits);
            }
        }
        else {
            // Non-contiguous; leave gaps with a zero (null) value
            std::fill(data, data + numWords, 0);
            for (auto & r_i: column.sparseIndexes) {
                ML::Bit_Writer<uint32_t> writer(data);
                writer.skip(r_i.first * indexBits);
                writer.write(r_i.second + 1, indexBits);
            }
        }
    }

    virtual bool forEach(const ForEachRowFn & onRow) const
    {
        ML::Bit_Extractor<uint32_t> bits(storage.get());

        for (size_t i = 0;  i < numEntries;  ++i) {
            int index = bits.extract<uint32_t>(indexBits);

            CellValue val;
            if (hasNulls) {
                if (index > 0)
                    val = table[index - 1];
            }
            else {
                val = table[index];
            }

            if (!onRow(i + firstEntry, val))
                return false;
        }

        return true;
    }

    virtual bool forEachDense(const ForEachRowFn & onRow) const
    {
        return forEach(onRow);
    }

    virtual CellValue get(uint32_t rowIndex) const
    {
        CellValue result;
        if (rowIndex < firstEntry)
            return result;
        rowIndex -= firstEntry;
        if (rowIndex >= numEntries)
            return result;
        ExcAssertLess(rowIndex, numEntries);
        ML::Bit_Extractor<uint32_t> bits(storage.get());
        bits.advance(rowIndex * indexBits);
        int index = bits.extract<uint32_t>(indexBits);
        if (hasNulls) {
            if (index == 0)
                return result;
            else return result = table[index - 1];
        }
        else {
            return result = table[index];
        }
    }

    virtual size_t size() const
    {
        return numEntries;
    }

    virtual size_t memusage() const
    {
        size_t result
            = sizeof(*this)
            + (indexBits * numEntries + 31) / 8;

        for (auto & v: table)
            result += v.memusage();

        return result;
    }

    virtual bool
    forEachDistinctValue(std::function<bool (const CellValue &)> fn) const
    {
        if (hasNulls) {
            if (!fn(CellValue()))
                return false;
        }
        for (auto & v: table) {
            if (!fn(v))
                return false;
        }

        return true;
    }

    std::shared_ptr<const uint32_t> storage;
    uint32_t indexBits;
    uint32_t numEntries;
    uint64_t firstEntry;
    
    bool hasNulls;
    std::vector<CellValue> table;
    ColumnTypes columnTypes;

    virtual ColumnTypes getColumnTypes() const
    {
        return columnTypes;
    }

    static size_t bytesRequired(const TabularDatasetColumn & column)
    {
        size_t numEntries = column.maxRowNumber - column.minRowNumber + 1;
        size_t hasNulls = column.sparseIndexes.size() < numEntries;
        int indexBits = ML::highest_bit(column.indexedVals.size() + hasNulls) + 1;
        size_t result
            = sizeof(TableFrozenColumn)
            + (indexBits * numEntries + 31) / 8;

        for (auto & v: column.indexedVals)
            result += v.memusage();

        return result;
    }
};

/// Sparse frozen column that finds each value in a lookup table
struct SparseTableFrozenColumn: public FrozenColumn {
    SparseTableFrozenColumn(TabularDatasetColumn & column)
        : table(column.indexedVals.size()), columnTypes(column.columnTypes)
    {
        firstEntry = column.minRowNumber;
        lastEntry = column.maxRowNumber;
        std::move(std::make_move_iterator(column.indexedVals.begin()),
                  std::make_move_iterator(column.indexedVals.end()),
                  table.begin());
        indexBits = ML::highest_bit(table.size()) + 1;
        rowNumBits = ML::highest_bit(column.maxRowNumber - column.minRowNumber) + 1;
        numEntries = column.sparseIndexes.size();
        size_t numWords = ((indexBits + rowNumBits) * numEntries + 31) / 32;
        uint32_t * data = new uint32_t[numWords];
        storage = std::shared_ptr<uint32_t>(data, [] (uint32_t * p) { delete[] p; });
            
        ML::Bit_Writer<uint32_t> writer(data);
        for (auto & i: column.sparseIndexes) {
            writer.write(i.first, rowNumBits);
            ExcAssertLess(i.second, table.size());
            writer.write(i.second, indexBits);
        }

#if 0
        size_t mem = memusage();
        if (mem > 30000) {
            using namespace std;
            cerr << "table with " << column.sparseIndexes.size()
                 << " entries from "
                 << column.minRowNumber << " to " << column.maxRowNumber
                 << " and " << table.size()
                 << " uniques takes " << mem << " memory" << endl;

            for (unsigned i = 0;  i < 5 && i < table.size();  ++i) {
                cerr << "  " << table[i] << endl;
            }
        }
#endif
    }

    virtual bool forEach(const ForEachRowFn & onRow) const
    {
        ML::Bit_Extractor<uint32_t> bits(storage.get());

        for (size_t i = 0;  i < numEntries;  ++i) {
            uint32_t rowNum = bits.extract<uint32_t>(rowNumBits);
            uint32_t index = bits.extract<uint32_t>(indexBits);
            if (!onRow(rowNum + firstEntry, table[index]))
                return false;
        }
        
        return true;
    }

    virtual bool forEachDense(const ForEachRowFn & onRow) const
    {
        ML::Bit_Extractor<uint32_t> bits(storage.get());

        size_t lastRowNum = 0;
        for (size_t i = 0;  i < numEntries;  ++i) {
            uint32_t rowNum = bits.extract<uint32_t>(rowNumBits);
            uint32_t index = bits.extract<uint32_t>(indexBits);

            while (lastRowNum < rowNum) {
                if (!onRow(firstEntry + lastRowNum, CellValue()))
                    return false;
                ++lastRowNum;
            }

            if (!onRow(firstEntry + rowNum, table[index]))
                return false;
        }

        while (firstEntry + lastRowNum <= lastEntry) {
            if (!onRow(firstEntry + lastRowNum, CellValue()))
                return false;
            ++lastRowNum;
        }
        
        return true;
    }

    virtual CellValue get(uint32_t rowIndex) const
    {
        CellValue result;
        if (rowIndex < firstEntry)
            return result;
        rowIndex -= firstEntry;

        auto getAtIndex = [&] (uint32_t n)
            {
                ML::Bit_Extractor<uint32_t> bits(storage.get());
                bits.advance(n * (indexBits + rowNumBits));
                uint32_t rowNum = bits.extract<uint32_t>(rowNumBits);
                uint32_t index = bits.extract<uint32_t>(indexBits);
                return std::make_pair(rowNum, index);
            };

        uint32_t first = 0;
        uint32_t last  = numEntries;

        while (first != last) {
            uint32_t middle = (first + last) / 2;
            uint32_t rowNum, index;
            std::tie(rowNum, index) = getAtIndex(middle);

            //cerr << "first = " << first << " middle = " << middle
            //     << " last = " << last << " rowNum = " << rowNum
            //     << " looking for " << rowIndex << endl;

            if (rowNum == rowIndex) {
                ExcAssertLess(index, table.size());
                return result = table[index];
            }

            // Break out if the element isn't there
            if (first + 1 == last)
                break;

            if (rowNum < rowIndex) {
                first = middle;
            }
            else {
                last = middle;
            }

        }
        
        return result;
    }

    virtual size_t size() const
    {
        return numEntries;
    }

    virtual size_t memusage() const
    {
        size_t result
            = sizeof(*this)
            + ((indexBits + rowNumBits) * numEntries + 31) / 8;

        for (auto & v: table)
            result += v.memusage();

        return result;
    }

    virtual bool
    forEachDistinctValue(std::function<bool (const CellValue &)> fn) const
    {
        if (!fn(CellValue()))
            return false;
        for (auto & v: table) {
            if (!fn(v))
                return false;
        }
        
        return true;
    }

    virtual ColumnTypes getColumnTypes() const
    {
        return columnTypes;
    }

    static size_t bytesRequired(const TabularDatasetColumn & column)
    {
        int indexBits = ML::highest_bit(column.indexedVals.size()) + 1;
        int rowNumBits = ML::highest_bit(column.maxRowNumber - column.minRowNumber) + 1;
        size_t numEntries = column.sparseIndexes.size();

        size_t result
            = sizeof(SparseTableFrozenColumn)
            + ((indexBits + rowNumBits) * numEntries + 31) / 8;

        for (auto & v: column.indexedVals)
            result += v.memusage();

        return result;
    }

    std::shared_ptr<const uint32_t> storage;
    compact_vector<CellValue, 0> table;
    uint8_t rowNumBits;
    uint8_t indexBits;
    uint32_t numEntries;
    size_t firstEntry;
    size_t lastEntry;  // WARNING: this is the number, not number + 1
    ColumnTypes columnTypes;
};

/// Frozen column that stores each value as a signed 64 bit integer
struct IntegerFrozenColumn: public FrozenColumn {

    struct SizingInfo {
        SizingInfo(const TabularDatasetColumn & column)
            : bytesRequired(-1)
        {
            if (!column.columnTypes.onlyIntegersAndNulls())
                return;  // can't use this column type
            if (column.columnTypes.maxPositiveInteger
                > (uint64_t)std::numeric_limits<int64_t>::max())
                return;  // out of range

            if (column.columnTypes.hasPositiveIntegers()
                && column.columnTypes.hasNegativeIntegers()) {
                range = column.columnTypes.maxPositiveInteger
                    - column.columnTypes.minNegativeInteger;
                offset = column.columnTypes.minNegativeInteger;
            }
            else if (column.columnTypes.hasPositiveIntegers()) {
                range = column.columnTypes.maxPositiveInteger
                    - column.columnTypes.minPositiveInteger;
                offset = column.columnTypes.minPositiveInteger;
            }
            else if (column.columnTypes.hasNegativeIntegers()) {
                range = column.columnTypes.maxNegativeInteger
                    - column.columnTypes.minNegativeInteger;
                offset = column.columnTypes.minNegativeInteger;
            }
            else {
                // only nulls or empty column; we can store another way
                return;
            }

            numEntries = column.maxRowNumber - column.minRowNumber + 1;
            hasNulls = column.sparseIndexes.size() < numEntries;

            // If we have too much range to represent nulls then we can't
            // use this kind of column.
            if (range == -1 && hasNulls)
                return;
                
#if 0 // later on... we should look for a common multiple to reduce bits used
   
            // Check for common multiple
            std::vector<int64_t> offsets;
            offsets.reserve(column.indexedVals.size());
            for (auto & v: column.indexedVals) {
                if (!v.empty())
                    offsets.emplace_back(v.toInt());
            }

            std::sort(offsets.begin(), offsets.end());
        
            // Find the multiple
            for (size_t i = 0;  i < offsets.size() - 1;  ++i) {
                offsets[i] = offsets[i + 1] - offsets[i];
            }
            if (!offsets.empty())
                offsets.pop_back();

            // Uniquify
            std::sort(offsets.begin(), offsets.end());
            offsets.erase(std::unique(offsets.begin(), offsets.end()),
                          offsets.end());
        
            static std::mutex mutex;
            std::unique_lock<std::mutex> guard(mutex);

            cerr << "got " << offsets.size() << " unique offsets starting at "
                 << offsets.front() << endl;

            for (size_t i = 0;  i < 100 && i < offsets.size() - 1;  ++i) {
                cerr << "  " << offsets[i];
            }
            cerr << endl;
#endif
            entryBits = ML::highest_bit(range + hasNulls) + 1;
            numWords = (entryBits * numEntries + 63) / 64;
            bytesRequired = sizeof(IntegerFrozenColumn) + numWords * 8;
        }

        operator ssize_t () const
        {
            return bytesRequired;
        }

        ssize_t bytesRequired;
        uint64_t range;
        int64_t offset;
        size_t numEntries;
        bool hasNulls;
        size_t numWords;
        int entryBits;
    };
    
    IntegerFrozenColumn(TabularDatasetColumn & column)
        : columnTypes(column.columnTypes)
    {
        SizingInfo info(column);
        ExcAssertNotEqual(info.bytesRequired, -1);

        firstEntry = column.minRowNumber;
        numEntries = info.numEntries;

        // Check it's really feasible
        ExcAssert(column.columnTypes.onlyIntegersAndNulls());
        ExcAssertLessEqual(column.columnTypes.maxPositiveInteger,
                           (uint64_t)std::numeric_limits<int64_t>::max());

        hasNulls = info.hasNulls;
        entryBits = info.entryBits;
        offset = info.offset;
        uint64_t * data = new uint64_t[info.numWords];
        storage = std::shared_ptr<uint64_t>(data, [] (uint64_t * p) { delete[] p; });

        if (!hasNulls) {
            // Contiguous rows
            //cerr << "fill with contiguous" << endl;
            ML::Bit_Writer<uint64_t> writer(data);
            for (size_t i = 0;  i < column.sparseIndexes.size();  ++i) {
                ExcAssertEqual(column.sparseIndexes[i].first, i);
                int64_t val
                    = column.indexedVals[column.sparseIndexes[i].second].toInt();
                //cerr << "writing " << val << " - " << offset << " = "
                //     << val - offset << " at " << i << endl;
                writer.write(val - offset, entryBits);
            }
        }
        else {
            // Non-contiguous; leave gaps with a zero (null) value
            std::fill(data, data + info.numWords, 0);
            for (auto & r_i: column.sparseIndexes) {
                int64_t val
                    = column.indexedVals[r_i.second].toInt();
                ML::Bit_Writer<uint64_t> writer(data);
                writer.skip(r_i.first * entryBits);
                writer.write(val - offset + 1, entryBits);
            }
        }

#if 0
        // Check that we got the right thing
        for (auto & i: column.sparseIndexes) {
            //cerr << "getting " << i.first << " with value "
            //     << column.indexedVals.at(i.second) << endl;
            ExcAssertEqual(get(i.first + firstEntry),
                           column.indexedVals.at(i.second));
        }
#endif
    }

    bool forEachImpl(const ForEachRowFn & onRow, bool keepNulls) const
    {
        ML::Bit_Extractor<uint64_t> bits(storage.get());

        for (size_t i = 0;  i < numEntries;  ++i) {
            int64_t val = bits.extract<uint64_t>(entryBits);
            if (hasNulls) {
                if (val == 0) {
                    if (keepNulls && !onRow(i + firstEntry, CellValue()))
                        return false;
                }
                else {
                    if (!onRow(i + firstEntry, val + offset - 1))
                        return false;
                }
            }
            else {
                if (!onRow(i + firstEntry, val + offset))
                    return false;
            }
        }

        return true;

    }

    virtual bool forEach(const ForEachRowFn & onRow) const
    {
        return forEachImpl(onRow, false /* keep nulls */);
    }

    virtual bool forEachDense(const ForEachRowFn & onRow) const
    {
        return forEachImpl(onRow, true /* keep nulls */);
    }

    virtual CellValue get(uint32_t rowIndex) const
    {
        CellValue result;
        if (rowIndex < firstEntry)
            return result;
        rowIndex -= firstEntry;
        if (rowIndex >= numEntries)
            return result;
        ExcAssertLess(rowIndex, numEntries);
        ML::Bit_Extractor<uint64_t> bits(storage.get());
        bits.advance(rowIndex * entryBits);
        int64_t val = bits.extract<uint64_t>(entryBits);
        if (hasNulls) {
            if (val == 0)
                return result;
            else return result = val + offset - 1;
        }
        else {
            //cerr << "got val " << val << " " << val + offset << endl;
            return result = val + offset;
        }
    }

    virtual size_t size() const
    {
        return numEntries;
    }

    virtual size_t memusage() const
    {
        size_t result
            = sizeof(*this)
            + (entryBits * numEntries + 63) / 8;

        return result;
    }

    virtual bool
    forEachDistinctValue(std::function<bool (const CellValue &)> fn) const
    {
        // Handle nulls first so we don't have to do them later
        if (hasNulls && !fn(CellValue()))
            return false;

        std::vector<int64_t> allVals;
        allVals.reserve(numEntries);

        ML::Bit_Extractor<uint64_t> bits(storage.get());
        
        for (size_t i = 0;  i < numEntries;  ++i) {
            int64_t val = bits.extract<uint64_t>(entryBits);
            if (val == 0 && hasNulls)
                continue;
            allVals.push_back(val);
            bits.advance(entryBits);
        }

        std::sort(allVals.begin(), allVals.end());
        auto endIt = std::unique(allVals.begin(), allVals.end());

        for (auto it = allVals.begin();  it != endIt;  ++it) {
            if (!fn(*it + offset - hasNulls))
                return false;
        }

        return true;
    }

    std::shared_ptr<const uint64_t> storage;
    uint32_t entryBits;
    uint32_t numEntries;
    uint64_t firstEntry;
    int64_t offset;

    bool hasNulls;
    ColumnTypes columnTypes;

    virtual ColumnTypes getColumnTypes() const
    {
        return columnTypes;
    }

    static ssize_t bytesRequired(const TabularDatasetColumn & column)
    {
        return SizingInfo(column);
    }
};

std::shared_ptr<FrozenColumn>
FrozenColumn::
freeze(TabularDatasetColumn & column)
{
    size_t required1 = TableFrozenColumn::bytesRequired(column);
    size_t required2 = SparseTableFrozenColumn::bytesRequired(column);
    size_t required3 = IntegerFrozenColumn::bytesRequired(column);

    if (required3 < std::min(required1, required2)) {
        return std::make_shared<IntegerFrozenColumn>(column);
        //cerr << "integer requires " << required3 << " instead of "
        //     << std::min(required1, required2) << endl;
    }

    if (required1 <= required2)
        return std::make_shared<TableFrozenColumn>(column);
    else return std::make_shared<SparseTableFrozenColumn>(column);
}

} // namespace MLDB


