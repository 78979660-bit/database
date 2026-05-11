#pragma once

#include <vector>
#include <memory>
#include <iostream>
#include "../BufferPoolManager.h"
#include "../Catalog/Schema.h"
#include "ColumnarPage.h"

namespace Database
{
    /**
     * ColumnStoreTable uses a columnar format, where each column's data is stored
     * in its own linked list of ColumnarPages. This makes read-only analytical queries
     * much faster, as they only need to fetch the specific column pages.
     */
    class ColumnStoreTable
    {
    public:
        ColumnStoreTable(BufferPoolManager *bpm, Schema *schema)
            : bpm_(bpm), schema_(schema)
        {
            // Initialize list of page pointers for each column
            uint32_t col_count = schema_->GetColumnCount();
            column_first_page_ids_.resize(col_count, INVALID_PAGE_ID);
            column_last_page_ids_.resize(col_count, INVALID_PAGE_ID);

            // Allocate initial pages for each column
            for (uint32_t i = 0; i < col_count; ++i)
            {
                page_id_t new_page_id;
                Page *page = bpm_->NewPage(&new_page_id);
                if (page != nullptr)
                {
                    ColumnarPage *col_page = reinterpret_cast<ColumnarPage *>(page);
                    // For now, assuming type size can be represented directly.
                    // E.g., int32_t is 4. VARCHAR might crash this simple prototype without Dictionary Encoding.
                    uint32_t type_size = schema_->GetColumn(i).GetFixedLength();
                    col_page->Init(new_page_id, type_size);

                    column_first_page_ids_[i] = new_page_id;
                    column_last_page_ids_[i] = new_page_id;

                    bpm_->UnpinPage(new_page_id, true);
                }
            }
        }

        // Insert a single tuple, split its contents across columns.
        bool InsertTuple(const std::vector<Value> &tuple)
        {
            if (tuple.size() != column_last_page_ids_.size())
            {
                return false;
            }

            for (size_t col_idx = 0; col_idx < tuple.size(); ++col_idx)
            {
                page_id_t last_page_id = column_last_page_ids_[col_idx];
                Page *page = bpm_->FetchPage(last_page_id);
                if (!page)
                    return false;

                ColumnarPage *col_page = reinterpret_cast<ColumnarPage *>(page);

                // Get binary representation of value
                // In a wider implementation, `Value` class should provide its underlying raw bytes
                // and size to serialize. Assuming `Value` has a SerializeTo method for now.
                const char *data_ptr = nullptr;
                uint32_t data_size = schema_->GetColumn(col_idx).GetFixedLength();

                // Simple integer handler for prototype purposes
                if (data_size == 4)
                {
                    int32_t int_val = tuple[col_idx].GetAs<int32_t>();
                    data_ptr = reinterpret_cast<const char *>(&int_val);
                    if (!col_page->InsertTuple(data_ptr, data_size))
                    {
                        // Allocation of new page needed
                        page_id_t new_page_id;
                        Page *new_raw_page = bpm_->NewPage(&new_page_id);
                        if (new_raw_page)
                        {
                            ColumnarPage *new_col_page = reinterpret_cast<ColumnarPage *>(new_raw_page);
                            new_col_page->Init(new_page_id, data_size);
                            new_col_page->InsertTuple(data_ptr, data_size);

                            col_page->SetNextPageId(new_page_id);
                            column_last_page_ids_[col_idx] = new_page_id;

                            bpm_->UnpinPage(new_page_id, true);
                        }
                    }
                }
                bpm_->UnpinPage(last_page_id, true);
            }
            return true;
        }

        page_id_t GetColumnFirstPageId(uint32_t col_idx) const
        {
            if (col_idx >= column_first_page_ids_.size())
                return INVALID_PAGE_ID;
            return column_first_page_ids_[col_idx];
        }

    private:
        BufferPoolManager *bpm_;
        Schema *schema_;

        // Pointers to the first and last page of each column's list
        std::vector<page_id_t> column_first_page_ids_;
        std::vector<page_id_t> column_last_page_ids_;
    };
}
