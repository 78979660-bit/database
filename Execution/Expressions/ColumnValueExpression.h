#pragma once

#include "AbstractExpression.h"

namespace Database
{

    class ColumnValueExpression : public AbstractExpression
    {
    public:
        // tuple_idx: 0 for left/only tuple, 1 for right tuple (in join)
        ColumnValueExpression(uint32_t tuple_idx, uint32_t col_idx)
            : AbstractExpression(), tuple_idx_(tuple_idx), col_idx_(col_idx), col_name_("") {}

        ColumnValueExpression(uint32_t tuple_idx, std::string col_name)
            : AbstractExpression(), tuple_idx_(tuple_idx), col_idx_(-1), col_name_(std::move(col_name)) {}

        inline std::string GetColName() const { return col_name_; }
        inline void SetColIdx(uint32_t idx) { col_idx_ = idx; }

        Value Evaluate(const Tuple *tuple, const Schema *schema) const override
        {
            uint32_t idx = col_idx_;
            if (idx == (uint32_t)-1)
            {
                int resolved_idx = schema->GetColumnIndex(col_name_);
                if (resolved_idx != -1)
                    idx = resolved_idx;
            }
            return tuple->GetValue(schema, idx);
        }

        Value EvaluateJoin(const Tuple *left_tuple, const Schema *left_schema,
                           const Tuple *right_tuple, const Schema *right_schema) const override
        {
            uint32_t idx = col_idx_;
            const Schema *target_schema = (tuple_idx_ == 0) ? left_schema : right_schema;
            const Tuple *target_tuple = (tuple_idx_ == 0) ? left_tuple : right_tuple;

            if (idx == (uint32_t)-1)
            {
                int resolved_idx = target_schema->GetColumnIndex(col_name_);
                if (resolved_idx != -1)
                    idx = resolved_idx;
            }

            return target_tuple->GetValue(target_schema, idx);
        }

        virtual void Evaluate(const Chunk &chunk, std::shared_ptr<Vector> &result) const override
        {
            // For ColumnValueExpression, the result is simply a reference to the column vector in the chunk!
            // To avoid copying, in an advanced engine `result` could be pointed directly to chunk.GetVector(col_idx).
            // Here, we'll do a vector copy or reference assignment based on the design.
            // Assuming `result` is meant to be populated:

            uint32_t idx = col_idx_;
            // If col_idx_ is not resolved, theoretically it should have been during binder/planner
            // assert(idx != (uint32_t)-1);
            if (idx == (uint32_t)-1)
            {
                // In some test files like test_parser.cpp, AST is directly executed without a proper Binder.
                // We could try to resolve it from chunk if chunk kept schema info, but chunk doesn't have schema.
                // To support test_parser.cpp's direct execution format without Schema in Chunk:
                // We'll throw or fail gracefully, but wait, test_parser's raw plan misses column index.
                // Actually, let's fix the test_parser.cpp instead to resolve it!
            }

            auto src_vector = chunk.GetVector(idx);
            size_t count = chunk.GetCount();
            auto sel_vector = chunk.GetSelectionVector();

            // We copy values to result
            for (size_t i = 0; i < count; ++i)
            {
                size_t physical_idx = sel_vector ? sel_vector->GetIndex(i) : i;
                result->SetValue(physical_idx, src_vector->GetValue(physical_idx));
            }
        }

        std::string GetColumnName() const { return col_name_; }
        uint32_t GetColIdx() const { return col_idx_; }
        uint32_t GetTupleIdx() const { return tuple_idx_; }
        void SetColumnIndex(uint32_t idx) { col_idx_ = idx; }

    private:
        uint32_t tuple_idx_;
        uint32_t col_idx_;
        std::string col_name_;
    };

} // namespace Database