#pragma once

#include "AbstractExecutor.h"
#include "../Plans/HashJoinPlanNode.h"
#include "../JIT/JitEngine.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>

namespace Database
{

    // 自定义 Value 的 Hash 函数，用于 unordered_map
    struct ValueHash
    {
        size_t operator()(const Value &val) const
        {
            if (val.GetTypeId() == TypeId::INTEGER)
            {
                return std::hash<int32_t>()(val.GetAsInteger());
            }
            else if (val.GetTypeId() == TypeId::VARCHAR)
            {
                return std::hash<std::string>()(val.GetAsVarchar());
            }
            return 0;
        }
    };

    class HashJoinExecutor : public AbstractExecutor
    {
    public:
        HashJoinExecutor(ExecutorContext *exec_ctx, const HashJoinPlanNode *plan,
                         std::unique_ptr<AbstractExecutor> &&left_child,
                         std::unique_ptr<AbstractExecutor> &&right_child)
            : AbstractExecutor(exec_ctx), plan_(plan),
              left_executor_(std::move(left_child)), right_executor_(std::move(right_child)) {}

        void Init() override
        {
            left_executor_->Init();
            right_executor_->Init();

            left_col_idx_ = plan_->GetLeftPlan()->GetOutputSchema()->GetColumnIndex(plan_->GetLeftJoinColName());
            right_col_idx_ = plan_->GetRightPlan()->GetOutputSchema()->GetColumnIndex(plan_->GetRightJoinColName());

            const AbstractExpression *predicate = plan_->GetPredicate();
            if (predicate && !compiled_batch_func_)
            {
                compiled_batch_func_ = jit_engine_.CompileBatchExpression(predicate);
            }

            // =========================
            // Build Phase (Vectorized)
            // =========================
            hash_table_.clear();

            Chunk left_chunk(STANDARD_VECTOR_SIZE);
            const auto left_schema = plan_->GetLeftPlan()->GetOutputSchema();
            for (uint32_t i = 0; i < left_schema->GetColumnCount(); ++i)
            {
                left_chunk.AddVector(std::make_shared<FlatVector<int32_t>>(left_schema->GetColumn(i).GetType(), STANDARD_VECTOR_SIZE));
            }

            while (left_executor_->Next(left_chunk))
            {
                auto key_vec = left_chunk.GetVector(left_col_idx_);
                size_t count = left_chunk.GetCount();
                for (size_t i = 0; i < count; ++i)
                {
                    Value left_val = key_vec->GetValue(i);
                    std::vector<Value> vals(left_schema->GetColumnCount());
                    for (size_t col = 0; col < left_schema->GetColumnCount(); ++col)
                    {
                        vals[col] = left_chunk.GetVector(col)->GetValue(i);
                    }
                    hash_table_[left_val].emplace_back(vals, left_schema);
                }
                left_chunk.Reset();
            }

            // =========================
            // Probe Phase Init
            // =========================
            right_chunk_ = Chunk(STANDARD_VECTOR_SIZE);
            const auto right_schema = plan_->GetRightPlan()->GetOutputSchema();
            for (uint32_t i = 0; i < right_schema->GetColumnCount(); ++i)
            {
                right_chunk_.AddVector(std::make_shared<FlatVector<int32_t>>(right_schema->GetColumn(i).GetType(), STANDARD_VECTOR_SIZE));
            }

            match_idx_ = 0;
            current_matches_ = nullptr;
            current_right_row_idx_ = 0;
            right_status_ = right_executor_->Next(right_chunk_);

            if (right_status_ && right_chunk_.GetCount() > 0)
            {
                Value right_val = right_chunk_.GetVector(right_col_idx_)->GetValue(current_right_row_idx_);
                auto it = hash_table_.find(right_val);
                if (it != hash_table_.end())
                {
                    current_matches_ = &(it->second);
                }
            }
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            while (true)
            {
                // 如果当前右表记录在哈希表中有匹配的左表记录，且尚未遍历完
                if (current_matches_ && match_idx_ < current_matches_->size())
                {
                    *tuple = AssembleJoinedTuple(&((*current_matches_)[match_idx_]), &right_tuple_);
                    *rid = RID(); // 返回一个虚拟的 RID，因为 JOIN 的结果不直接对应物理页中的单行
                    match_idx_++;
                    return true;
                }

                // 否则，移动右表指针拉取下一条右表记录
                right_status_ = right_executor_->Next(&right_tuple_, &right_rid_);
                if (!right_status_)
                {
                    return false; // 右表也已经扫描到底了，结束
                }

                // 拿着新的右表记录的关键字段，去哈希表里探测
                match_idx_ = 0;
                current_matches_ = nullptr;
                Value right_val = right_tuple_.GetValue(plan_->GetRightPlan()->GetOutputSchema(), right_col_idx_);
                auto it = hash_table_.find(right_val);
                if (it != hash_table_.end())
                {
                    current_matches_ = &(it->second);
                }
            }
        }

        // Vectorized Next
        bool Next(Chunk &chunk) override
        {
            size_t output_count = 0;
            const size_t batch_size = chunk.GetCapacity();

            while (output_count < batch_size)
            {
                while (current_matches_ && match_idx_ < current_matches_->size() && output_count < batch_size)
                {
                    const Tuple &left_tuple = (*current_matches_)[match_idx_];
                    const Schema *left_schema = plan_->GetLeftPlan()->GetOutputSchema();
                    const Schema *right_schema = plan_->GetRightPlan()->GetOutputSchema();

                    size_t out_col_idx = 0;
                    for (uint32_t i = 0; i < left_schema->GetColumnCount(); ++i)
                    {
                        chunk.GetVector(out_col_idx++)->SetValue(output_count, left_tuple.GetValue(left_schema, i));
                    }
                    for (uint32_t i = 0; i < right_schema->GetColumnCount(); ++i)
                    {
                        chunk.GetVector(out_col_idx++)->SetValue(output_count, right_chunk_.GetVector(i)->GetValue(current_right_row_idx_));
                    }

                    output_count++;
                    match_idx_++;
                }

                if (output_count == batch_size)
                    break;

                // Advance to next right row
                if (right_status_)
                {
                    current_right_row_idx_++;
                    while (current_right_row_idx_ >= right_chunk_.GetCount())
                    {
                        right_chunk_.Reset();
                        right_status_ = right_executor_->Next(right_chunk_);
                        if (!right_status_ || right_chunk_.GetCount() == 0)
                        {
                            right_status_ = false;
                            break;
                        }
                        current_right_row_idx_ = 0;
                    }
                }

                if (!right_status_)
                    break; // EOF

                match_idx_ = 0;
                current_matches_ = nullptr;
                Value right_val = right_chunk_.GetVector(right_col_idx_)->GetValue(current_right_row_idx_);
                auto it = hash_table_.find(right_val);
                if (it != hash_table_.end())
                {
                    current_matches_ = &(it->second);
                }
            }

            if (output_count == 0)
                return false;
            chunk.SetCount(output_count);

            const AbstractExpression *predicate = plan_->GetPredicate();
            if (predicate == nullptr)
            {
                return true;
            }

            // Apply JIT filtering
            size_t matched_count = 0;
            auto sel_vector = chunk.GetSelectionVector();
            if (!sel_vector)
            {
                sel_vector = std::make_shared<SelectionVector>(batch_size);
                for (size_t i = 0; i < output_count; ++i)
                    sel_vector->SetIndex(i, i);
            }
            auto new_sel_vector = std::make_shared<SelectionVector>(batch_size);

            if (compiled_batch_func_)
            {
                std::vector<const void *> cols;
                for (size_t col_idx = 0; col_idx < chunk.GetColumnCount(); ++col_idx)
                {
                    auto flat_vec = std::dynamic_pointer_cast<FlatVector<int32_t>>(chunk.GetVector(col_idx));
                    cols.push_back(flat_vec->Data());
                }
                std::vector<int32_t> results(output_count);
                compiled_batch_func_(cols.data(), results.data(), nullptr, output_count);

                for (size_t i = 0; i < output_count; ++i)
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
                for (size_t i = 0; i < output_count; ++i)
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

            // If everything was filtered out, recursively fetch next batch
            return Next(chunk);
        }

    private:
        Tuple AssembleJoinedTuple(const Tuple *left, const Tuple *right)
        {
            std::vector<Value> values;

            const Schema *left_schema = plan_->GetLeftPlan()->GetOutputSchema();
            for (uint32_t i = 0; i < left_schema->GetColumnCount(); ++i)
            {
                values.push_back(left->GetValue(left_schema, i));
            }

            const Schema *right_schema = plan_->GetRightPlan()->GetOutputSchema();
            for (uint32_t i = 0; i < right_schema->GetColumnCount(); ++i)
            {
                values.push_back(right->GetValue(right_schema, i));
            }

            return Tuple(values, plan_->GetOutputSchema());
        }

        const HashJoinPlanNode *plan_;
        std::unique_ptr<AbstractExecutor> left_executor_;
        std::unique_ptr<AbstractExecutor> right_executor_;

        int left_col_idx_{-1};
        int right_col_idx_{-1};

        // 哈希表: Hash Key (数据库 Value) -> 属于该 Key 的所有左表记录的列表
        std::unordered_map<Value, std::vector<Tuple>, ValueHash> hash_table_;

        // 探测阶段探测状态标识
        bool right_status_{false};
        Tuple right_tuple_;
        RID right_rid_;

        std::vector<Tuple> *current_matches_{nullptr};
        size_t match_idx_{0};

        Chunk right_chunk_{0};
        size_t current_right_row_idx_{0};

        JitEngine jit_engine_;
        JitEngine::CompiledBatchFunc compiled_batch_func_{nullptr};
    };

} // namespace Database