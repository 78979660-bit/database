#pragma once

#include "AbstractExecutor.h"
#include "../Plans/ValuesPlanNode.h"
#include "../../Storage/Table/ColumnarTable.h"

namespace Database
{

    class ValuesExecutor : public AbstractExecutor
    {
    public:
        ValuesExecutor(ExecutorContext *exec_ctx, const ValuesPlanNode *plan)
            : AbstractExecutor(exec_ctx), plan_(plan), cursor_(0) {}

        void Init() override
        {
            cursor_ = 0;
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            if (cursor_ >= plan_->GetValues().size())
            {
                return false;
            }

            const auto &values = plan_->GetValues()[cursor_];
            *tuple = Tuple(values, plan_->GetOutputSchema());
            cursor_++;
            return true;
        }

        // Vectorized Next
        bool Next(Chunk &chunk) override
        {
            const auto &all_values = plan_->GetValues();
            if (cursor_ >= all_values.size())
            {
                return false;
            }

            size_t row_count = 0;
            const size_t batch_size = chunk.GetCapacity();
            const auto schema = plan_->GetOutputSchema();

            // Setup chunk vectors if needed (assuming they are pre-allocated or we create them)
            // Here we assume vectors are pre-allocated by caller or engine based on schema,
            // we just fill them.
            while (cursor_ < all_values.size() && row_count < batch_size)
            {
                const auto &values = all_values[cursor_];
                for (size_t col_idx = 0; col_idx < values.size(); ++col_idx)
                {
                    chunk.GetVector(col_idx)->SetValue(row_count, values[col_idx]);
                }
                cursor_++;
                row_count++;
            }

            chunk.SetCount(row_count);
            return row_count > 0;
        }

    private:
        const ValuesPlanNode *plan_;
        size_t cursor_;
    };

} // namespace Database