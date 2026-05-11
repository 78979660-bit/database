#pragma once

#include <cstring>
#include <iostream>
#include <cstdint>
#include <vector>
#include "../Page.h"
#include "../Catalog/Schema.h"
#include "../Type/Value.h"

namespace Database
{
    enum class CompressionType : uint8_t
    {
        None = 0,
        RLE = 1,
        Dictionary = 2,
        Delta = 3
    };

    /**
     * @brief PAXPage (Partition Attributes Across) is a hybrid row-column page layout.
     * It splits a traditional page into multiple Mini-pages, one for each column,
     * to maximize cache locality for analytical queries while remaining aligned in pages.
     *
     * PAXPage Layout:
     * -------------------------------------------------------------------------
     * | PageId (4) | LSN (4) | ColumnCount (4) | NumTuples (4) | NextPage (4) |
     * -------------------------------------------------------------------------
     * | ColumnMeta[0] | ColumnMeta[1] | ... | ColumnMeta[ColumnCount - 1]     |
     * -------------------------------------------------------------------------
     * | Mini-page 0 (Data for Col 0)                                          |
     * -------------------------------------------------------------------------
     * | Mini-page 1 (Data for Col 1)                                          |
     * -------------------------------------------------------------------------
     * | ...                                                                   |
     * -------------------------------------------------------------------------
     */
    class PAXPage : public Page
    {
    public:
        static constexpr int HEADER_SIZE = 64; // Meta, LSN, NumTuples, SlotCount, etc.
        static constexpr int MAX_COLUMNS = 32;

        struct ColumnMeta
        {
            uint32_t offset;      // mini-page offset
            uint32_t length;      // total allocated byte length
            CompressionType comp; // None, Dictionary, RLE, Delta
        };

        void Init(page_id_t page_id, const Schema &schema)
        {
            SetPageId(page_id);
            SetLSN(0);
            SetNumTuples(0);
            SetNextPageId(INVALID_PAGE_ID);

            uint32_t col_count = schema.GetColumnCount();
            SetColumnCount(col_count);

            // Calculate the total fixed size of a tuple to find proportions
            uint32_t total_fixed_size = 0;
            for (uint32_t i = 0; i < col_count; ++i)
            {
                uint32_t len = schema.GetColumn(i).GetFixedLength();
                total_fixed_size += (len == 0 ? 16 : len); // Assume 16 bytes for variable length defaults
            }

            uint32_t available_space = PAGE_SIZE - HEADER_SIZE - (col_count * sizeof(ColumnMeta));
            uint32_t offset = HEADER_SIZE + col_count * sizeof(ColumnMeta);

            for (uint32_t i = 0; i < col_count; ++i)
            {
                ColumnMeta meta;
                meta.offset = offset;
                meta.comp = CompressionType::None;

                uint32_t len = schema.GetColumn(i).GetFixedLength();
                uint32_t effective_len = (len == 0 ? 16 : len);

                // Allocate proportional space based on column size
                meta.length = static_cast<uint32_t>((static_cast<double>(effective_len) / total_fixed_size) * available_space);
                SetColumnMeta(i, meta);

                offset += meta.length;
            }
        }

        // --- Getters & Setters ---
        page_id_t GetNextPageId() const { return *reinterpret_cast<const page_id_t *>(GetData() + NEXT_PAGE_ID_OFFSET); }
        void SetNextPageId(page_id_t next_page_id) { *reinterpret_cast<page_id_t *>(GetData() + NEXT_PAGE_ID_OFFSET) = next_page_id; }

        uint32_t GetColumnCount() const { return *reinterpret_cast<const uint32_t *>(GetData() + COLUMN_COUNT_OFFSET); }
        void SetColumnCount(uint32_t count) { *reinterpret_cast<uint32_t *>(GetData() + COLUMN_COUNT_OFFSET) = count; }

        uint32_t GetNumTuples() const { return *reinterpret_cast<const uint32_t *>(GetData() + NUM_TUPLES_OFFSET); }
        void SetNumTuples(uint32_t count) { *reinterpret_cast<uint32_t *>(GetData() + NUM_TUPLES_OFFSET) = count; }

        ColumnMeta GetColumnMeta(uint32_t col_idx) const
        {
            return *reinterpret_cast<const ColumnMeta *>(GetData() + HEADER_SIZE + col_idx * sizeof(ColumnMeta));
        }

        void SetColumnMeta(uint32_t col_idx, const ColumnMeta &meta)
        {
            *reinterpret_cast<ColumnMeta *>(GetData() + HEADER_SIZE + col_idx * sizeof(ColumnMeta)) = meta;
        }

        char *GetMiniPageData(uint32_t col_idx)
        {
            ColumnMeta meta = GetColumnMeta(col_idx);
            return GetData() + meta.offset;
        }

        /**
         * @brief Insert a generic Tuple into this PAX page.
         * Appends the tuple's column values to their respective mini-pages.
         * Converts row format to column format seamlessly!
         */
        bool InsertTuple(const std::vector<Value> &values, const Schema &schema)
        {
            uint32_t num_tuples = GetNumTuples();
            uint32_t col_count = GetColumnCount();

            // Simple boundary check: assure we don't overwrite mini-pages
            // (A fully implemented engine would check exact byte displacements per column)
            for (uint32_t i = 0; i < col_count; ++i)
            {
                ColumnMeta meta = GetColumnMeta(i);
                uint32_t field_size = schema.GetColumn(i).GetFixedLength() == 0 ? 8 : schema.GetColumn(i).GetFixedLength(); // Approx 8 bytes for simple handling
                if (meta.offset + (num_tuples + 1) * field_size > (i == col_count - 1 ? PAGE_SIZE : GetColumnMeta(i + 1).offset))
                {
                    return false; // Page is full for this column
                }
            }

            // Scatter values to respective Mini-pages
            for (uint32_t i = 0; i < col_count; ++i)
            {
                ColumnMeta meta = GetColumnMeta(i);
                char *mini_page = GetData() + meta.offset;
                uint32_t field_size = schema.GetColumn(i).GetFixedLength() == 0 ? 8 : schema.GetColumn(i).GetFixedLength();

                // Write native value directly to column buffer
                // This converts row-inserts to columnar storage physically
                values[i].SerializeTo(mini_page + num_tuples * field_size);
            }

            SetNumTuples(num_tuples + 1);
            return true;
        }

    private:
        static constexpr int PAGE_ID_OFFSET = 0;
        static constexpr int LSN_OFFSET = 4;
        static constexpr int COLUMN_COUNT_OFFSET = 8;
        static constexpr int NUM_TUPLES_OFFSET = 12;
        static constexpr int NEXT_PAGE_ID_OFFSET = 16;

        void SetPageId(page_id_t page_id) { *reinterpret_cast<page_id_t *>(GetData() + PAGE_ID_OFFSET) = page_id; }
        void SetLSN(lsn_t lsn) { *reinterpret_cast<lsn_t *>(GetData() + LSN_OFFSET) = lsn; }
    };
}