#pragma once

#include "AbstractPlanNode.h"
#include "../Expressions/AbstractExpression.h"
#include "../Expressions/AbstractExpression.h"
#include "../Expressions/AbstractExpression.h"
#include <string>
#include <utility>

namespace Database
{

    class HashJoinPlanNode : public AbstractPlanNode
    {
    public:
        // 等值连接条件: left_col = right_col
        HashJoinPlanNode(const std::shared_ptr<const Schema> &output_schema,
                         const AbstractPlanNode *left_child,
                         const AbstractPlanNode *right_child,
                         std::string left_col_name,
                         std::string right_col_name,
                         const AbstractExpression *predicate = nullptr)
            : AbstractPlanNode(output_schema, {left_child, right_child}),
              left_col_name_(std::move(left_col_name)),
              right_col_name_(std::move(right_col_name)),
              predicate_(predicate) {}

        PlanType GetType() const override { return PlanType::HashJoin; }

        const AbstractPlanNode *GetLeftPlan() const { return GetChildAt(0); }
        const AbstractPlanNode *GetRightPlan() const { return GetChildAt(1); }

        const std::string &GetLeftJoinColName() const { return left_col_name_; }
        const std::string &GetRightJoinColName() const { return right_col_name_; }
        const AbstractExpression *GetPredicate() const { return predicate_; }

    private:
        std::string left_col_name_;
        std::string right_col_name_;
        const AbstractExpression *predicate_{nullptr};
    };

} // namespace Database