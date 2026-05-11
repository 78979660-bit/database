#pragma once

#include "AbstractExpression.h"

namespace Database
{

    class ConstantValueExpression : public AbstractExpression
    {
    public:
        ConstantValueExpression(const Value &val) : AbstractExpression(), val_(val) {}

        Value Evaluate(const Tuple *tuple, const Schema *schema) const override
        {
            return val_;
        }

        Value EvaluateJoin(const Tuple *left_tuple, const Schema *left_schema,
                           const Tuple *right_tuple, const Schema *right_schema) const override
        {
            return val_;
        }

        virtual void Evaluate(const Chunk &chunk, std::shared_ptr<Vector> &result) const override
        {
            size_t count = chunk.GetCount();
            auto sel_vector = chunk.GetSelectionVector();

            // Broad-cast constant value to all selected rows
            for (size_t i = 0; i < count; ++i)
            {
                size_t physical_idx = sel_vector ? sel_vector->GetIndex(i) : i;
                result->SetValue(physical_idx, val_);
            }
        }

        Value GetValue() const { return val_; }

    private:
        Value val_;
    };

} // namespace Database