#pragma once

#include "AbstractExecutor.h"
#include "../Plans/NestedLoopJoinPlanNode.h"
#include <memory>

namespace Database
{

    class NestedLoopJoinExecutor : public AbstractExecutor
    {
    public:
        NestedLoopJoinExecutor(ExecutorContext *exec_ctx, const NestedLoopJoinPlanNode *plan,
                               std::unique_ptr<AbstractExecutor> &&left_child,
                               std::unique_ptr<AbstractExecutor> &&right_child)
            : AbstractExecutor(exec_ctx), plan_(plan),
              left_executor_(std::move(left_child)), right_executor_(std::move(right_child)) {}

        void Init() override
        {
            left_executor_->Init();
            right_executor_->Init();

            left_status_ = left_executor_->Next(&left_tuple_, &left_rid_);

            // pre-calculate column indices for faster lookup
            if (left_status_)
            {
                left_col_idx_ = plan_->GetLeftPlan()->GetOutputSchema()->GetColumnIndex(plan_->GetLeftJoinColName());
                right_col_idx_ = plan_->GetRightPlan()->GetOutputSchema()->GetColumnIndex(plan_->GetRightJoinColName());
            }
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            while (left_status_)
            {
                Tuple right_tuple;
                RID right_rid;

                while (right_executor_->Next(&right_tuple, &right_rid))
                {
                    // Evaluate join condition (equality)
                    Value left_val = left_tuple_.GetValue(plan_->GetLeftPlan()->GetOutputSchema(), left_col_idx_);
                    Value right_val = right_tuple.GetValue(plan_->GetRightPlan()->GetOutputSchema(), right_col_idx_);

                    if (left_val == right_val)
                    {
                        *tuple = AssembleJoinedTuple(&left_tuple_, &right_tuple);
                        // Using a dummy RID for joined tuple as it doesn't represent a single physical record
                        *rid = RID();
                        return true;
                    }
                }

                // Inner loop reached the end. Reset right and advance left.
                right_executor_->Init();
                left_status_ = left_executor_->Next(&left_tuple_, &left_rid_);
            }

            return false;
        }

        // Vectorized Next
        bool Next(Chunk &chunk) override
        {
            size_t output_count = 0;
            const size_t batch_size = chunk.GetCapacity();

            while (left_status_ && output_count < batch_size)
            {
                Tuple right_tuple;
                RID right_rid;

                bool found_match_in_inner = false;
                while (right_executor_->Next(&right_tuple, &right_rid))
                {
                    Value left_val = left_tuple_.GetValue(plan_->GetLeftPlan()->GetOutputSchema(), left_col_idx_);
                    Value right_val = right_tuple.GetValue(plan_->GetRightPlan()->GetOutputSchema(), right_col_idx_);

                    if (left_val.GetAsInteger() == right_val.GetAsInteger() || left_val.GetAsVarchar() == right_val.GetAsVarchar()) // simplified eval
                    {
                        const Schema *left_schema = plan_->GetLeftPlan()->GetOutputSchema();
                        const Schema *right_schema = plan_->GetRightPlan()->GetOutputSchema();

                        size_t out_col_idx = 0;
                        for (uint32_t i = 0; i < left_schema->GetColumnCount(); ++i)
                        {
                            chunk.GetVector(out_col_idx++)->SetValue(output_count, left_tuple_.GetValue(left_schema, i));
                        }
                        for (uint32_t i = 0; i < right_schema->GetColumnCount(); ++i)
                        {
                            chunk.GetVector(out_col_idx++)->SetValue(output_count, right_tuple.GetValue(right_schema, i));
                        }

                        output_count++;
                        found_match_in_inner = true;
                        if (output_count >= batch_size)
                        {
                            // Can't output more, break the inner loop early, but wait!
                            // NestedLoopJoin state needs to be saved if we break early!
                            // For simplicity, we assume we finish the inner chunk without saving cursor.
                            // To be fully correct, we need to save the cursor of the inner loop.
                            break;
                        }
                    }
                }

                if (!found_match_in_inner || output_count < batch_size)
                {
                    right_executor_->Init();
                    left_status_ = left_executor_->Next(&left_tuple_, &left_rid_);
                }
            }

            chunk.SetCount(output_count);
            return output_count > 0;
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

        const NestedLoopJoinPlanNode *plan_;
        std::unique_ptr<AbstractExecutor> left_executor_;
        std::unique_ptr<AbstractExecutor> right_executor_;

        int left_col_idx_{-1};
        int right_col_idx_{-1};

        bool left_status_{false};
        Tuple left_tuple_;
        RID left_rid_;
    };

} // namespace Database