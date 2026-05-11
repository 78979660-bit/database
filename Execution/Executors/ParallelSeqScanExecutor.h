#pragma once

#include "AbstractExecutor.h"
#include "../Plans/SeqScanPlanNode.h"
#include "../../Storage/Table/ColumnarTable.h"
#include "../../Concurrency/Visibility.h"
#include "../JIT/JitEngine.h"
#include "../ExecutionThreadPool.h"
#include "../Morsel.h"
#include <mutex>

namespace Database
{
    /**
     * ParallelSeqScanExecutor shares a central ColumnarTable::TableIterator across multiple thread tasks.
     * Threads concurrently grab the next batch of data (Morsel) until exhaustion.
     */
    class ParallelSeqScanExecutor : public AbstractExecutor
    {
    public:
        ParallelSeqScanExecutor(ExecutorContext *exec_ctx, const SeqScanPlanNode *plan)
            : AbstractExecutor(exec_ctx), plan_(plan), shared_iter_(nullptr, 0) {}
        void Init() override
        {
            std::string table_name = plan_->GetTableName();
            TableMetadata *metadata = exec_ctx_->GetCatalog()->GetTable(table_name);
            if (metadata)
            {
                shared_iter_ = metadata->columnar_storage_->MakeIterator();
                columnar_table_ = metadata->columnar_storage_.get();
            }
            is_exhausted_ = false;

            const AbstractExpression *predicate = plan_->GetPredicate();
            if (predicate && !compiled_func_ && !compiled_batch_func_)
            {
                compiled_func_ = jit_engine_.CompileExpression(predicate);
                compiled_batch_func_ = jit_engine_.CompileBatchExpression(predicate);
            }
        }
        bool Next(Tuple *tuple, RID *rid) override
        {
            // Fallback for single row
            if (!columnar_table_)
                return false;

            std::lock_guard<std::mutex> lock(iter_mutex_);
            Transaction *txn = exec_ctx_->GetTransaction();

            while (shared_iter_ != columnar_table_->MakeEofIterator())
            {
                Tuple curr_tuple = *shared_iter_;
                RID curr_rid = shared_iter_.GetRID();
                ++shared_iter_;

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
                        if (val.GetAsInteger() == 0)
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
            if (!columnar_table_ || is_exhausted_.load())
                return false;

            size_t row_count = 0;
            const size_t batch_size = chunk.GetCapacity();
            Transaction *txn = exec_ctx_->GetTransaction();
            const AbstractExpression *predicate = plan_->GetPredicate();

            while (true)
            {
                std::vector<Tuple> local_tuples;
                std::vector<RID> local_rids;
                local_tuples.reserve(batch_size);
                local_rids.reserve(batch_size);
                row_count = 0;

                {
                    std::lock_guard<std::mutex> lock(iter_mutex_);
                    while (shared_iter_ != columnar_table_->MakeEofIterator() && row_count < batch_size)
                    {
                        Tuple curr_tuple = *shared_iter_;
                        RID curr_rid = shared_iter_.GetRID();
                        ++shared_iter_;

                        if (txn != nullptr)
                        {
                            if (!Visibility::IsVisibleTo(txn, curr_tuple.GetMeta()))
                            {
                                continue;
                            }
                        }

                        local_tuples.push_back(curr_tuple);
                        local_rids.push_back(curr_rid);
                        row_count++;
                    }

                    if (shared_iter_ == columnar_table_->MakeEofIterator())
                    {
                        is_exhausted_ = true;
                    }
                }

                if (local_tuples.empty())
                {
                    return false;
                }

                chunk.Reset();
                const auto &schema = plan_->OutputSchema();
                for (size_t i = 0; i < local_tuples.size(); ++i)
                {
                    for (size_t col_idx = 0; col_idx < schema->GetColumnCount(); ++col_idx)
                    {
                        Value val = local_tuples[i].GetValue(schema.get(), col_idx);
                        chunk.GetVector(col_idx)->SetValue(i, val);
                    }
                    chunk.SetRID(i, local_rids[i]);
                }
                chunk.SetCount(local_tuples.size());

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
                    for (size_t i = 0; i < local_tuples.size(); ++i)
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

                    std::vector<int32_t> results(local_tuples.size());
                    std::vector<uint8_t> nulls(local_tuples.size(), 0);
                    compiled_batch_func_((const void **)cols.data(), results.data(), nulls.data(), local_tuples.size());

                    for (size_t i = 0; i < local_tuples.size(); ++i)
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

                    for (size_t i = 0; i < local_tuples.size(); ++i)
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

                if (is_exhausted_.load())
                {
                    return false;
                }
            }
            return false;
        }

    private:
        JitEngine jit_engine_;
        JitEngine::CompiledExpressionFunc compiled_func_{nullptr};
        JitEngine::CompiledBatchFunc compiled_batch_func_{nullptr};

        const SeqScanPlanNode *plan_;

        // These shared structures constitute our primitive 'Morsel Dispenser'
        std::mutex iter_mutex_;
        ColumnarTable::TableIterator shared_iter_;
        std::atomic<bool> is_exhausted_{false};

        ColumnarTable *columnar_table_ = nullptr;
    };

} // namespace Database