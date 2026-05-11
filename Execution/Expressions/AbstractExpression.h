#pragma once

#include <vector>
#include <memory>
#include "../../Storage/Tuple/Tuple.h"
#include "../../Catalog/Schema.h"
#include "../../Type/Value.h"
#include "../Chunk.h"

namespace Database
{

    class AbstractExpression
    {
    public:
        AbstractExpression(std::vector<std::shared_ptr<AbstractExpression>> children = {})
            : children_(std::move(children)) {}
        virtual ~AbstractExpression() = default;

        // Legacy Row-based Evaluation
        virtual Value Evaluate(const Tuple *tuple, const Schema *schema) const = 0;

        virtual Value EvaluateJoin(const Tuple *left_tuple, const Schema *left_schema,
                                   const Tuple *right_tuple, const Schema *right_schema) const
        {
            return Evaluate(left_tuple, left_schema);
        }

        // Vectorized Batch Evaluation
        virtual void Evaluate(const Chunk &chunk, std::shared_ptr<Vector> &result) const
        {
            // Default fallback using row-based evaluate.
            // Concrete expressions should override this to use fast vector loops.
            size_t count = chunk.GetCount();
            auto sel_vector = chunk.GetSelectionVector();
            for (size_t i = 0; i < count; ++i)
            {
                size_t physical_idx = sel_vector ? sel_vector->GetIndex(i) : i;

                // Construct a temporary tuple (highly inefficient!)
                std::vector<Value> row_values;
                row_values.reserve(chunk.GetColumnCount());
                for (size_t col_idx = 0; col_idx < chunk.GetColumnCount(); ++col_idx)
                {
                    row_values.push_back(chunk.GetVector(col_idx)->GetValue(physical_idx));
                }

                // Assuming schema is not strictly required if we construct Tuple with full vector
                // But this fallback is heavily limited without proper schema.
                // We'll trust that derived classes won't hit this fallback if properly implemented.
                Tuple temp_tuple(row_values, nullptr);
                Value val = Evaluate(&temp_tuple, nullptr);

                result->SetValue(physical_idx, val);
            }
        }

        virtual void EvaluateJoin(const Chunk &left_chunk, const Chunk &right_chunk, std::shared_ptr<Vector> &result) const
        {
            // Join evaluate over chunks...
            // Omitted generic fallback for simplicity
        }

        const AbstractExpression *GetChildAt(size_t index) const
        {
            return children_[index].get();
        }

        const std::vector<std::shared_ptr<AbstractExpression>> &GetChildren() const
        {
            return children_;
        }

    protected:
        std::vector<std::shared_ptr<AbstractExpression>> children_;
    };

} // namespace Database