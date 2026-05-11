#pragma once

#include "AbstractPlanNode.h"
#include <string>
#include <memory>
#include "../Expressions/AbstractExpression.h"

namespace Database
{

    class FilterPlanNode : public AbstractPlanNode
    {
    public:
        FilterPlanNode(const std::shared_ptr<const Schema> &output_schema, const AbstractPlanNode *child, std::shared_ptr<AbstractExpression> predicate)
            : AbstractPlanNode(output_schema, {child}), predicate_(std::move(predicate)) {}

        PlanType GetType() const override { return PlanType::Filter; }

        const AbstractPlanNode *GetChildPlan() const { return GetChildAt(0); }

        const AbstractExpression *GetPredicate() const { return predicate_.get(); }

    private:
        std::shared_ptr<AbstractExpression> predicate_;
    };

} // namespace Database