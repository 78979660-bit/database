#pragma once

#include "AbstractPlanNode.h"
#include <string>
#include <utility>

namespace Database
{

    class NestedLoopJoinPlanNode : public AbstractPlanNode
    {
    public:
        // 等值连接条件: left_col = right_col
        NestedLoopJoinPlanNode(const std::shared_ptr<const Schema> &output_schema,
                               const AbstractPlanNode *left_child,
                               const AbstractPlanNode *right_child,
                               std::string left_col_name,
                               std::string right_col_name)
            : AbstractPlanNode(output_schema, {left_child, right_child}),
              left_col_name_(std::move(left_col_name)),
              right_col_name_(std::move(right_col_name)) {}

        PlanType GetType() const override { return PlanType::NestedLoopJoin; }

        const AbstractPlanNode *GetLeftPlan() const { return GetChildAt(0); }
        const AbstractPlanNode *GetRightPlan() const { return GetChildAt(1); }

        const std::string &GetLeftJoinColName() const { return left_col_name_; }
        const std::string &GetRightJoinColName() const { return right_col_name_; }

    private:
        std::string left_col_name_;
        std::string right_col_name_;
    };

} // namespace Database