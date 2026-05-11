#pragma once

#include "AbstractExpression.h"
#include <stdexcept>
#include <memory>

namespace Database
{
    class SelectStatement; // Forward declaration

    class InSubqueryExpression : public AbstractExpression
    {
    public:
        InSubqueryExpression(std::shared_ptr<AbstractExpression> left,
                             std::shared_ptr<SelectStatement> subquery)
            : AbstractExpression({std::move(left)}), subquery_(std::move(subquery)) {}

        Value Evaluate(const Tuple *tuple, const Schema *schema) const override
        {
            throw std::runtime_error("InSubqueryExpression cannot be evaluated without executor context.");
        }

        Value EvaluateJoin(const Tuple *left_tuple, const Schema *left_schema,
                           const Tuple *right_tuple, const Schema *right_schema) const override
        {
            throw std::runtime_error("InSubqueryExpression cannot be evaluated without executor context.");
        }

        virtual void Evaluate(const Chunk &chunk, std::shared_ptr<Vector> &result) const override
        {
            throw std::runtime_error("InSubqueryExpression cannot be evaluated without executor context.");
        }

        std::shared_ptr<SelectStatement> GetSubquery() const { return subquery_; }

    private:
        std::shared_ptr<SelectStatement> subquery_;
    };

} // namespace Database