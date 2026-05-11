#pragma once

#include "AbstractExpression.h"
#include "../SIMD/SIMDVectorOperations.h"

namespace Database
{

    enum class CompType
    {
        Equal,
        NotEqual,
        LessThan,
        GreaterThan,
        LessThanOrEqual,
        GreaterThanOrEqual
    };

    class ComparisonExpression : public AbstractExpression
    {
    public:
        ComparisonExpression(std::shared_ptr<AbstractExpression> left,
                             std::shared_ptr<AbstractExpression> right,
                             CompType comp_type)
            : AbstractExpression({std::move(left), std::move(right)}), comp_type_(comp_type) {}

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
            std::shared_ptr<Vector> left_result = std::make_shared<FlatVector<Value>>(TypeId::INVALID, chunk.GetCapacity());
            children_[0]->Evaluate(chunk, left_result);

            std::shared_ptr<Vector> right_result = std::make_shared<FlatVector<Value>>(TypeId::INVALID, chunk.GetCapacity());
            children_[1]->Evaluate(chunk, right_result);

            size_t count = chunk.GetCount();
            auto sel_vector = chunk.GetSelectionVector();

            for (size_t i = 0; i < count; ++i)
            {
                size_t physical_idx = sel_vector ? sel_vector->GetIndex(i) : i; 
                Value lhs = left_result->GetValue(physical_idx);
                Value rhs = right_result->GetValue(physical_idx);
                Value comp_res = PerformComputation(lhs, rhs);
                result->SetValue(physical_idx, comp_res);
            }
        }

        CompType GetCompType() const { return comp_type_; }

    private:
        Value PerformComputation(const Value &lhs, const Value &rhs) const      
        {
            int res = 0;
            switch (comp_type_)
            {
            case CompType::Equal:
                res = (lhs == rhs);
                break;
            case CompType::LessThan:
                res = (lhs < rhs);
                break;
            case CompType::GreaterThan:
                res = (rhs < lhs);
                break;
            case CompType::NotEqual:
                res = !(lhs == rhs);
                break;
            case CompType::LessThanOrEqual:
                res = (lhs < rhs) || (lhs == rhs);
                break;
            case CompType::GreaterThanOrEqual:
                res = (rhs < lhs) || (lhs == rhs);
                break;
            }
            return Value(static_cast<int32_t>(res));
        }

        CompType comp_type_;
    };

} // namespace Database
