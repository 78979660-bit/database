#pragma once

#include "AbstractExpression.h"

namespace Database
{

    enum class ArithType
    {
        Add,
        Subtract,
        Multiply,
        Divide
    };

    class ArithmeticExpression : public AbstractExpression
    {
    public:
        ArithmeticExpression(std::shared_ptr<AbstractExpression> left,
                             std::shared_ptr<AbstractExpression> right,
                             ArithType arith_type)
            : AbstractExpression({std::move(left), std::move(right)}), arith_type_(arith_type) {}

        Value Evaluate(const Tuple *tuple, const Schema *schema) const override
        {
            Value lhs = children_[0]->Evaluate(tuple, schema);
            Value rhs = children_[1]->Evaluate(tuple, schema);
            return PerformComputation(lhs, rhs);
        }

        Value EvaluateJoin(const Tuple *left_tuple, const Schema *left_schema,
                           const Tuple *right_tuple, const Schema *right_schema) const override
        {
            Value lhs = children_[0]->EvaluateJoin(left_tuple, left_schema, right_tuple, right_schema);
            Value rhs = children_[1]->EvaluateJoin(left_tuple, left_schema, right_tuple, right_schema);
            return PerformComputation(lhs, rhs);
        }

        virtual void Evaluate(const Chunk &chunk, std::shared_ptr<Vector> &result) const override
        {
            std::shared_ptr<Vector> left_result = std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, chunk.GetCapacity());
            children_[0]->Evaluate(chunk, left_result);

            std::shared_ptr<Vector> right_result = std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, chunk.GetCapacity());
            children_[1]->Evaluate(chunk, right_result);

            size_t count = chunk.GetCount();
            auto sel_vector = chunk.GetSelectionVector();

            for (size_t i = 0; i < count; ++i)
            {
                size_t physical_idx = sel_vector ? sel_vector->GetIndex(i) : i;

                Value lhs = left_result->GetValue(physical_idx);
                Value rhs = right_result->GetValue(physical_idx);

                Value arith_res = PerformComputation(lhs, rhs);
                result->SetValue(physical_idx, arith_res);
            }
        }

        ArithType GetArithType() const { return arith_type_; }

    private:
        Value PerformComputation(const Value &lhs, const Value &rhs) const
        {
            int32_t left_val = lhs.GetAsInteger();
            int32_t right_val = rhs.GetAsInteger();
            int32_t res = 0;

            switch (arith_type_)
            {
            case ArithType::Add:
                res = left_val + right_val;
                break;
            case ArithType::Subtract:
                res = left_val - right_val;
                break;
            case ArithType::Multiply:
                res = left_val * right_val;
                break;
            case ArithType::Divide:
                res = right_val != 0 ? left_val / right_val : 0;
                break;
            }
            return Value(res);
        }

        ArithType arith_type_;
    };

} // namespace Database