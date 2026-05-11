#pragma once

#include <memory>
#include "OptimizerRule.h"
#include "LogicalPlanNode.h"
#include "../Execution/Expressions/LogicalExpression.h"
#include "../Execution/Expressions/ColumnValueExpression.h"

namespace Database
{

    class FilterMergeRule : public OptimizerRule
    {
    public:
        std::shared_ptr<AbstractLogicalPlanNode> Apply(
            std::shared_ptr<AbstractLogicalPlanNode> plan,
            Catalog *catalog) const override
        {
            if (plan->GetType() == LogicalPlanType::Filter)
            {
                auto current_filter = std::static_pointer_cast<LogicalFilter>(plan);
                if (current_filter->GetChildren().size() == 1 && current_filter->GetChildAt(0)->GetType() == LogicalPlanType::Filter)
                {
                    auto child_filter = std::static_pointer_cast<LogicalFilter>(current_filter->GetChildAt(0));
                    auto new_predicate = std::make_shared<LogicalExpression>(
                        current_filter->predicate_,
                        child_filter->predicate_,
                        LogicType::AND);
                    auto new_filter = std::make_shared<LogicalFilter>(new_predicate);
                    for (size_t i = 0; i < child_filter->GetChildren().size(); ++i)
                    {
                        new_filter->AddChild(child_filter->GetChildAt(i));
                    }
                    return new_filter;
                }
            }
            return plan;
        }

        std::string GetName() const override { return "FilterMergeRule"; }
    };

    class PredicatePushDownRule : public OptimizerRule
    {
    private:
        void ExtractTuplesIdx(const AbstractExpression *expr, bool &has_left, bool &has_right) const
        {
            if (auto col_expr = dynamic_cast<const ColumnValueExpression *>(expr))
            {
                if (col_expr->GetTupleIdx() == 0) has_left = true;
                if (col_expr->GetTupleIdx() == 1) has_right = true;
            }
            for (auto &child : expr->GetChildren())
            {
                ExtractTuplesIdx(child.get(), has_left, has_right);
            }
        }

    public:
        std::shared_ptr<AbstractLogicalPlanNode> Apply(
            std::shared_ptr<AbstractLogicalPlanNode> plan,
            Catalog *catalog) const override
        {
            if (plan->GetType() == LogicalPlanType::Filter && plan->GetChildren().size() == 1 && plan->GetChildAt(0)->GetType() == LogicalPlanType::Join)
            {
                auto filter = std::static_pointer_cast<LogicalFilter>(plan);
                auto join = std::static_pointer_cast<LogicalJoin>(plan->GetChildAt(0));

                bool has_left = false;
                bool has_right = false;
                ExtractTuplesIdx(filter->predicate_.get(), has_left, has_right);

                // If predicate only depends on the left child of the join
                if (has_left && !has_right)
                {
                    auto new_filter = std::make_shared<LogicalFilter>(filter->predicate_);
                    new_filter->AddChild(join->GetChildAt(0));
                    
                    auto new_join = std::make_shared<LogicalJoin>(join->join_type_, join->on_condition_);
                    new_join->AddChild(new_filter);
                    new_join->AddChild(join->GetChildAt(1));
                    return new_join;
                }
                // If predicate only depends on the right child of the join
                else if (!has_left && has_right)
                {
                    auto new_filter = std::make_shared<LogicalFilter>(filter->predicate_);
                    new_filter->AddChild(join->GetChildAt(1));
                    
                    auto new_join = std::make_shared<LogicalJoin>(join->join_type_, join->on_condition_);
                    new_join->AddChild(join->GetChildAt(0));
                    new_join->AddChild(new_filter);
                    return new_join;
                }
            }
            return plan;
        }

        std::string GetName() const override { return "PredicatePushDownRule"; }
    };

} // namespace Database
