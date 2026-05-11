#pragma once

#include "AbstractExecutor.h"
#include "../../Concurrency/Transaction.h"
#include "../Plans/SeqScanPlanNode.h"
#include "../../Storage/Table/ColumnarTable.h"
#include "../../Concurrency/Visibility.h"
#include "../JIT/JitEngine.h"

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
        JitEngine jit_engine_;
        JitEngine::CompiledExpressionFunc compiled_func_{nullptr};
        JitEngine::CompiledBatchFunc compiled_batch_func_{nullptr};

        const SeqScanPlanNode *plan_;
        ColumnarTable::TableIterator iter_;
        ColumnarTable *columnar_table_ = nullptr;
    };

} // namespace Database
