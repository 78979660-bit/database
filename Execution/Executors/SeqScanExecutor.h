#pragma once

#include "AbstractExecutor.h"
#include "../../Concurrency/Transaction.h"
#include "../Plans/SeqScanPlanNode.h"
#include "../../Storage/Table/ColumnarTable.h"
#include "../../Concurrency/Visibility.h"
#include "../JIT/JitEngine.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace Database
{

    class SeqScanExecutor : public AbstractExecutor
    {
    public:
        SeqScanExecutor(ExecutorContext *exec_ctx, const SeqScanPlanNode *plan)
            : AbstractExecutor(exec_ctx), plan_(plan), iter_(nullptr, 0) {}
        void Init() override
        {
            std::string table_name = plan_->GetTableName();
            TableMetadata *metadata = exec_ctx_->GetCatalog()->GetTable(table_name);
            if (metadata)
            {
                Transaction *txn = exec_ctx_->GetTransaction();
                LockManager *lock_manager = exec_ctx_->GetLockManager();
                if (txn != nullptr && lock_manager != nullptr && txn->GetIsolationLevel() != IsolationLevel::READ_UNCOMMITTED)
                {
                    lock_manager->LockTable(txn, LockMode::SHARED, metadata->oid_);
                }

                iter_ = metadata->columnar_storage_->MakeIterator();
                columnar_table_ = metadata->columnar_storage_.get();
                scan_row_ = 0;
            }

            const AbstractExpression *predicate = plan_->GetPredicate();
            if (predicate && !compiled_func_ && !compiled_batch_func_)
            {
                compiled_func_ = jit_engine_.CompileExpression(predicate);
                compiled_batch_func_ = jit_engine_.CompileBatchExpression(predicate);
            }
        }
        bool Next(Tuple *tuple, RID *rid) override
        {
            if (!columnar_table_)
                return false;

            Transaction *txn = exec_ctx_->GetTransaction();
            LockManager *lock_manager = exec_ctx_->GetLockManager();

            auto end_iter = columnar_table_->MakeEofIterator();
            while (iter_ != end_iter)
            {
                Tuple curr_tuple = *iter_;
                RID curr_rid = iter_.GetRID();
                ++iter_;

                if (txn != nullptr)
                {
                    if (!Visibility::IsVisibleTo(txn, curr_tuple.GetMeta()))
                    {
                        continue;
                    }
                }

                // Apply JIT Filter if exists
                if (compiled_func_)
                {
                    const int32_t *row_data = reinterpret_cast<const int32_t *>(curr_tuple.GetData());
                    int32_t res;
                    uint8_t is_null;
                    compiled_func_(row_data, &res, &is_null);
                    if (is_null)
                        res = 0;
                    if (res == 0)
                    {
                        continue;
                    }
                }
                else
                {
                    const AbstractExpression *predicate = plan_->GetPredicate();
                    if (predicate != nullptr)
                    {
                        Value val = predicate->Evaluate(&curr_tuple, plan_->OutputSchema().get());
                        if (val.GetAsInteger() == 0) // Treat 0 as false
                        {
                            continue;
                        }
                    }
                }

                *tuple = curr_tuple;
                *rid = curr_rid;
                return true;
            }
            return false;
        }
        // Vectorized Next
        bool Next(Chunk &chunk) override
        {
            if (!columnar_table_)
                return false;

            Transaction *txn = exec_ctx_->GetTransaction();
            const size_t batch_size = chunk.GetCapacity();

            const AbstractExpression *predicate = plan_->GetPredicate();
            if (txn == nullptr && CanUseFastColumnarPath(chunk))
            {
                return NextFastColumnarBatch(chunk, predicate);
            }

            auto end_iter = columnar_table_->MakeEofIterator();
            while (iter_ != end_iter)
            {
                size_t row_count = 0;
                chunk.Reset();

                // A typical implementation would decode Tuple data into FlatVectors directly
                while (iter_ != end_iter && row_count < batch_size)
                {
                    Tuple curr_tuple = *iter_;
                    // Notice RID vector would be needed as well if other executor need to use RIDs
                    ++iter_;

                    if (txn != nullptr)
                    {
                        if (!Visibility::IsVisibleTo(txn, curr_tuple.GetMeta()))
                        {
                            continue;
                        }
                    }

                    const auto &schema = plan_->OutputSchema();
                    for (size_t col_idx = 0; col_idx < schema->GetColumnCount(); ++col_idx)
                    {
                        Value val = curr_tuple.GetValue(schema.get(), col_idx);
                        chunk.GetVector(col_idx)->SetValue(row_count, val);
                    }
                    chunk.SetRID(row_count, curr_tuple.GetRID());
                    row_count++;
                }

                if (row_count == 0)
                {
                    return false;
                }

                chunk.SetCount(row_count);

                // No predicate, just return the chunk
                if (predicate == nullptr)
                {
                    return true;
                }

                // Apply Predicate filtering over the Chunk
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
                // If matched_count is 0, we should continue reading next chunk, so it loop back to the top
            }

            return false;
        }

    private:
        bool CanUseFastColumnarPath(Chunk &chunk)
        {
            const auto &schema = plan_->OutputSchema();
            if (!schema)
            {
                return false;
            }

            for (size_t col_idx = 0; col_idx < schema->GetColumnCount(); ++col_idx)
            {
                if (schema->GetColumn(col_idx).GetType() != TypeId::INTEGER)
                {
                    return false;
                }
            }

            if (chunk.GetColumnCount() == 0)
            {
                for (size_t col_idx = 0; col_idx < schema->GetColumnCount(); ++col_idx)
                {
                    chunk.AddVector(std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, chunk.GetCapacity()));
                }
            }
            return chunk.GetColumnCount() == schema->GetColumnCount();
        }

        bool NextFastColumnarBatch(Chunk &chunk, const AbstractExpression *predicate)
        {
            const size_t batch_size = chunk.GetCapacity();
            const size_t total_rows = columnar_table_->GetRowCount();
            const size_t col_count = chunk.GetColumnCount();

            if (cols_.size() != col_count)
            {
                cols_.assign(col_count, nullptr);
                unpack_buffers_.assign(col_count, std::vector<int32_t>(batch_size));
            }
            for (auto &buffer : unpack_buffers_)
            {
                if (buffer.size() < batch_size)
                {
                    buffer.resize(batch_size);
                }
            }
            if (results_.size() < batch_size)
            {
                results_.resize(batch_size);
                nulls_.resize(batch_size);
            }
            if (!selection_vector_ || selection_capacity_ < batch_size)
            {
                selection_vector_ = std::make_shared<SelectionVector>(batch_size);
                selection_capacity_ = batch_size;
            }

            while (scan_row_ < total_rows)
            {
                const size_t row_count = std::min(batch_size, total_rows - scan_row_);
                chunk.Reset();

                for (size_t col_idx = 0; col_idx < col_count; ++col_idx)
                {
                    auto flat_vec = std::static_pointer_cast<FlatVector<int32_t>>(chunk.GetVector(col_idx));
                    int32_t *dest = flat_vec->Data();
                    const int32_t *src = columnar_table_->UnpackColumnBatch(static_cast<uint32_t>(col_idx), scan_row_, row_count, unpack_buffers_[col_idx].data());
                    if (src != dest)
                    {
                        std::memcpy(dest, src, row_count * sizeof(int32_t));
                    }
                    cols_[col_idx] = dest;
                }

                for (size_t i = 0; i < row_count; ++i)
                {
                    chunk.SetRID(i, RID(0, static_cast<uint32_t>(scan_row_ + i)));
                }

                scan_row_ += row_count;
                chunk.SetCount(row_count);

                if (predicate == nullptr)
                {
                    return true;
                }

                size_t matched_count = 0;
                if (compiled_batch_func_)
                {
                    std::fill(nulls_.begin(), nulls_.begin() + row_count, 0);
                    compiled_batch_func_(reinterpret_cast<const void **>(cols_.data()), results_.data(), nulls_.data(), row_count);

                    for (size_t i = 0; i < row_count; ++i)
                    {
                        if (nulls_[i] == 0 && results_[i] != 0)
                        {
                            selection_vector_->SetIndex(matched_count++, i);
                        }
                    }
                }
                else
                {
                    if (!eval_result_ || eval_result_->GetCapacity() < batch_size)
                    {
                        eval_result_ = std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, batch_size);
                        eval_vector_ = eval_result_;
                    }
                    predicate->Evaluate(chunk, eval_vector_);

                    for (size_t i = 0; i < row_count; ++i)
                    {
                        Value val = eval_result_->GetValue(i);
                        if (!val.IsNull() && val.GetAsInteger() != 0)
                        {
                            selection_vector_->SetIndex(matched_count++, i);
                        }
                    }
                }

                if (matched_count > 0)
                {
                    chunk.SetCount(matched_count);
                    chunk.SetSelectionVector(selection_vector_);
                    return true;
                }
            }

            return false;
        }

        JitEngine jit_engine_;
        JitEngine::CompiledExpressionFunc compiled_func_{nullptr};
        JitEngine::CompiledBatchFunc compiled_batch_func_{nullptr};

        const SeqScanPlanNode *plan_;
        ColumnarTable::TableIterator iter_;
        ColumnarTable *columnar_table_ = nullptr;
        size_t scan_row_ = 0;
        size_t selection_capacity_ = 0;
        std::vector<const int32_t *> cols_;
        std::vector<std::vector<int32_t>> unpack_buffers_;
        std::vector<int32_t> results_;
        std::vector<uint8_t> nulls_;
        std::shared_ptr<SelectionVector> selection_vector_;
        std::shared_ptr<FlatVector<int32_t>> eval_result_;
        std::shared_ptr<Vector> eval_vector_;
    };

} // namespace Database
