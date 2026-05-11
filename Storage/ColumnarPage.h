#pragma once

#include <cstring>
#include <iostream>
#include <cstdint>
#include "../Page.h"
#include "../Type/Value.h"

namespace Database
{
    /**
     * ColumnarPage Layout:
     * ------------------------------------------------------------------
     * | Header | (PageId, LSN, NumTuples, TupleSize, NextPageId, CompressionType) |
     * ------------------------------------------------------------------
     * | Null Bitmap | (Optional, indicates null fields)                 |
     * ------------------------------------------------------------------
     * | Data[0] | Data[1] | ... | Data[NumTuples - 1]                  |
     * ------------------------------------------------------------------
     */
    enum class CompressionType : uint8_t
    {
        None = 0,
        RLE = 1,
        Dictionary = 2
    };

    class ColumnarPage : public Page
    {
    public:
        void Init(page_id_t page_id, uint32_t tuple_size, CompressionType comp_type = CompressionType::None)
        {
            SetPageId(page_id);
            SetLSN(0);
            SetNumTuples(0);
            SetTupleSize(tuple_size);
            SetNextPageId(INVALID_PAGE_ID);
            SetCompressionType(comp_type);
        }

        // --- Getters & Setters ---

        uint32_t GetNumTuples() const { return *reinterpret_cast<const uint32_t *>(GetData() + NUM_TUPLES_OFFSET); }
        void SetNumTuples(uint32_t count) { *reinterpret_cast<uint32_t *>(GetData() + NUM_TUPLES_OFFSET) = count; }

        uint32_t GetTupleSize() const { return *reinterpret_cast<const uint32_t *>(GetData() + TUPLE_SIZE_OFFSET); }
        void SetTupleSize(uint32_t size) { *reinterpret_cast<uint32_t *>(GetData() + TUPLE_SIZE_OFFSET) = size; }

        page_id_t GetNextPageId() const { return *reinterpret_cast<const page_id_t *>(GetData() + NEXT_PAGE_ID_OFFSET); }
        void SetNextPageId(page_id_t next_page_id) { *reinterpret_cast<page_id_t *>(GetData() + NEXT_PAGE_ID_OFFSET) = next_page_id; }

        CompressionType GetCompressionType() const { return *reinterpret_cast<const CompressionType *>(GetData() + COMPRESSION_TYPE_OFFSET); }
        void SetCompressionType(CompressionType type) { *reinterpret_cast<CompressionType *>(GetData() + COMPRESSION_TYPE_OFFSET) = type; }

        // --- Tuple Access ---

        bool InsertTuple(const char *data, uint32_t tuple_size)
        {
            if (tuple_size != GetTupleSize())
                return false;

            uint32_t num_tuples = GetNumTuples();
            uint32_t data_offset = DATA_ARRAY_OFFSET + num_tuples * tuple_size;

            // Check if we have enough space
            if (data_offset + tuple_size > PAGE_SIZE)
            {
                return false; // Page is full
            }

            // Copy data and increment count
            memcpy(GetData() + data_offset, data, tuple_size);
            SetNumTuples(num_tuples + 1);

            return true;
        }

        bool GetTuple(uint32_t index, char *buffer) const
        {
            if (index >= GetNumTuples())
                return false;

            uint32_t tuple_size = GetTupleSize();
            uint32_t data_offset = DATA_ARRAY_OFFSET + index * tuple_size;

            memcpy(buffer, GetData() + data_offset, tuple_size);
            return true;
        }

    private:
        static constexpr int PAGE_ID_OFFSET = 0;
        static constexpr int LSN_OFFSET = 4;
        static constexpr int NUM_TUPLES_OFFSET = 8;
        static constexpr int TUPLE_SIZE_OFFSET = 12;
        static constexpr int NEXT_PAGE_ID_OFFSET = 16;
        static constexpr int COMPRESSION_TYPE_OFFSET = 20;
        static constexpr int DATA_ARRAY_OFFSET = 21;

        void SetPageId(page_id_t page_id) { *reinterpret_cast<page_id_t *>(GetData() + PAGE_ID_OFFSET) = page_id; }
        void SetLSN(lsn_t lsn) { *reinterpret_cast<lsn_t *>(GetData() + LSN_OFFSET) = lsn; }
    };
}
