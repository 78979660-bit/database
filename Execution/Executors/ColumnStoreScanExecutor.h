#pragma once

#include "AbstractExecutor.h"
#include "../Plans/SeqScanPlanNode.h"
#include "../../Storage/ColumnStoreTable.h"
#include "../../Concurrency/Visibility.h"
#include "../JIT/JitEngine.h"
#include "../JIT/JitEngine.h"
#include <vector>
#include <algorithm>

namespace Database
{

    class ColumnStoreScanExecutor : public AbstractExecutor
    {
    public:
        ColumnStoreScanExecutor(ExecutorContext *exec_ctx, const SeqScanPlanNode *plan)
            : AbstractExecutor(exec_ctx), plan_(plan), column_table_(nullptr) {}

        void Init() override
        {
            std::string table_name = plan_->GetTableName();
            TableMetadata *metadata = exec_ctx_->GetCatalog()->GetTable(table_name);
            const AbstractExpression *predicate = plan_->GetPredicate();
            if (predicate && !compiled_batch_func_)
            {
                compiled_batch_func_ = jit_engine_.CompileBatchExpression(predicate);
            }

            if (metadata && metadata->column_table_)
            {
                column_table_ = metadata->column_table_.get();
                uint32_t col_count = plan_->GetOutputSchema()->GetColumnCount();
                cursors_.resize(col_count);

                for (uint32_t i = 0; i < col_count; ++i)
                {
                    cursors_[i].current_page_id = column_table_->GetColumnFirstPageId(i);
                    cursors_[i].current_index = 0;

                    if (cursors_[i].current_page_id != INVALID_PAGE_ID)
                    {
                        Page *page = exec_ctx_->GetBufferPoolManager()->FetchPage(cursors_[i].current_page_id);
                        if (page)
                        {
                            ColumnarPage *col_page = reinterpret_cast<ColumnarPage *>(page);
                            cursors_[i].current_page_num_tuples = col_page->GetNumTuples();
                            exec_ctx_->GetBufferPoolManager()->UnpinPage(cursors_[i].current_page_id, false);
                        }
                        else
                        {
                            cursors_[i].current_page_num_tuples = 0;
                        }
                    }
                    else
                    {
                        cursors_[i].current_page_num_tuples = 0;
                    }
                }
            }
        }

        // Row-based access is supported but sub-optimal
        bool Next(Tuple *tuple, RID *rid) override
        {
            throw std::runtime_error("ColumnStoreScanExecutor supports vectorization only.");
            return false;
        }

        // Vectorized Next
        bool Next(Chunk &chunk) override
        {
            if (!column_table_ || cursors_.empty())
                return false;

            const size_t batch_size = chunk.GetCapacity();
            const auto &schema = plan_->GetOutputSchema();
            uint32_t col_count = schema->GetColumnCount();
            BufferPoolManager *bpm = exec_ctx_->GetBufferPoolManager();
            const AbstractExpression *predicate = plan_->GetPredicate();

            while (true)
            {
                if (cursors_[0].current_page_id == INVALID_PAGE_ID)
                    return false;

                size_t row_count = 0;
                chunk.Reset();

                while (row_count < batch_size)
                {
                    if (cursors_[0].current_page_id == INVALID_PAGE_ID)
                        break;

                    uint32_t available_in_page = cursors_[0].current_page_num_tuples - cursors_[0].current_index;

                    if (available_in_page == 0)
                    {
                        bool reached_end = false;
                        for (uint32_t c = 0; c < col_count; ++c)
                        {
                            Page *page = bpm->FetchPage(cursors_[c].current_page_id);
                            if (page)
                            {
                                ColumnarPage *col_page = reinterpret_cast<ColumnarPage *>(page);
                                page_id_t next_page_id = col_page->GetNextPageId();
                                bpm->UnpinPage(cursors_[c].current_page_id, false);

                                cursors_[c].current_page_id = next_page_id;
                                cursors_[c].current_index = 0;

                                if (next_page_id != INVALID_PAGE_ID)
                                {
                                    Page *next_page = bpm->FetchPage(next_page_id);
                                    if (next_page)
                                    {
                                        ColumnarPage *next_col_page = reinterpret_cast<ColumnarPage *>(next_page);
                                        cursors_[c].current_page_num_tuples = next_col_page->GetNumTuples();
                                        bpm->UnpinPage(next_page_id, false);
                                    }
                                    else
                                    {
                                        reached_end = true;
                                    }
                                }
                                else
                                {
                                    reached_end = true;
                                }
                            }
                            else
                            {
                                reached_end = true;
                            }
                        }
                        if (reached_end)
                            break;
                        available_in_page = cursors_[0].current_page_num_tuples;
                        if (available_in_page == 0)
                            break;
                    }

                    uint32_t read_limit = std::min<uint32_t>(batch_size - row_count, available_in_page);

                    for (uint32_t c = 0; c < col_count; ++c)
                    {
                        Page *page = bpm->FetchPage(cursors_[c].current_page_id);
                        if (page)
                        {
                            ColumnarPage *col_page = reinterpret_cast<ColumnarPage *>(page);
                            TypeId type = schema->GetColumn(c).GetType();

                            auto vector = chunk.GetVector(c);

                            if (type == TypeId::INTEGER)
                            {
                                auto flat_vec = std::static_pointer_cast<FlatVector<int32_t>>(vector);
                                int32_t *dest_data = flat_vec->Data();

                                for (uint32_t i = 0; i < read_limit; ++i)
                                {
                                    int32_t val_buf;
                                    col_page->GetTuple(cursors_[c].current_index + i, reinterpret_cast<char *>(&val_buf));
                                    dest_data[row_count + i] = val_buf;
                                    flat_vec->SetNull(row_count + i, false);
                                }
                            }
                            else
                            {
                                for (uint32_t i = 0; i < read_limit; ++i)
                                {
                                    int32_t val_buf;
                                    col_page->GetTuple(cursors_[c].current_index + i, reinterpret_cast<char *>(&val_buf));
                                    vector->SetValue(row_count + i, Value(val_buf));
                                }
                            }

                            bpm->UnpinPage(cursors_[c].current_page_id, false);
                        }
                    }

                    for (uint32_t c = 0; c < col_count; ++c)
                    {
                        cursors_[c].current_index += read_limit;
                    }
                    row_count += read_limit;
                }

                if (row_count == 0)
                {
                    return false;
                }

                chunk.SetCount(row_count);

                if (predicate == nullptr)
                {
                    return true;
                }

                size_t matched_count = 0;
                auto sel_vector = chunk.GetSelectionVector();
                if (!sel_vector)
                {
                    sel_vector = std::make_shared<SelectionVector>(batch_size);
                    for (size_t i = 0; i < row_count; ++i)
                    {
                        sel_vector->SetIndex(i, i);
                    }
                }
                auto new_sel_vector = std::make_shared<SelectionVector>(batch_size);

                if (compiled_batch_func_)
                {
                    std::vector<const int32_t *> cols;
                    cols.reserve(chunk.GetColumnCount());
                    for (size_t col_idx = 0; col_idx < chunk.GetColumnCount(); ++col_idx)
                    {
                        auto flat_vec = std::dynamic_pointer_cast<FlatVector<int32_t>>(chunk.GetVector(col_idx));
                        cols.push_back(flat_vec->Data());
                    }

                    std::vector<int32_t> results(row_count);
                    std::vector<uint8_t> nulls(row_count, 0);
                    compiled_batch_func_((const void **)cols.data(), results.data(), nulls.data(), row_count);

                    for (size_t i = 0; i < row_count; ++i)
                    {
                        size_t physical_idx = sel_vector->GetIndex(i);
                        if (results[i] != 0)
                        {
                            new_sel_vector->SetIndex(matched_count++, physical_idx);
                        }
                    }
                }
                else
                {
                    std::shared_ptr<Vector> eval_result = std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, batch_size);
                    predicate->Evaluate(chunk, eval_result);

                    for (size_t i = 0; i < row_count; ++i)
                    {
                        size_t physical_idx = sel_vector->GetIndex(i);
                        Value val = eval_result->GetValue(physical_idx);
                        if (!val.IsNull() && val.GetAsInteger() != 0)
                        {
                            new_sel_vector->SetIndex(matched_count++, physical_idx);
                        }
                    }
                }

                if (matched_count > 0)
                {
                    chunk.SetCount(matched_count);
                    chunk.SetSelectionVector(new_sel_vector);
                    return true;
                }
            }
            return false;
        }

    private:
        struct ColumnCursor
        {
            page_id_t current_page_id;
            uint32_t current_index;
            uint32_t current_page_num_tuples;
        };

        const SeqScanPlanNode *plan_;
        ColumnStoreTable *column_table_;
        std::vector<ColumnCursor> cursors_;
        JitEngine jit_engine_;
        JitEngine::CompiledBatchFunc compiled_batch_func_{nullptr};
    };

} // namespace Database