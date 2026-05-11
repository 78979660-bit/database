#pragma once

#include <memory>
#include <vector>
#include "AbstractPlanNode.h"
#include "../Expressions/AbstractExpression.h"

namespace Database
{

    enum class AggregationType
    {
        CountStar,
        Count,
        Sum,
        Min,
        Max
    };

    class AggregationPlanNode : public AbstractPlanNode
    {
    public:
        AggregationPlanNode(std::shared_ptr<const Schema> output_schema,
                            const AbstractPlanNode *child,
                            std::vector<std::shared_ptr<AbstractExpression>> group_bys,
                            std::vector<std::shared_ptr<AbstractExpression>> aggregates,
                            std::vector<AggregationType> agg_types)
            : AbstractPlanNode(std::move(output_schema), {child}),
              group_bys_(std::move(group_bys)),
              aggregates_(std::move(aggregates)),
              agg_types_(std::move(agg_types))
        {
        }

        PlanType GetType() const override { return PlanType::Aggregation; }

        const AbstractPlanNode *GetChildPlan() const { return GetChildAt(0); }

        const std::vector<std::shared_ptr<AbstractExpression>> &GetGroupBys() const { return group_bys_; }
        const std::vector<std::shared_ptr<AbstractExpression>> &GetAggregates() const { return aggregates_; }
        const std::vector<AggregationType> &GetAggregateTypes() const { return agg_types_; }

    private:
        std::vector<std::shared_ptr<AbstractExpression>> group_bys_;
        std::vector<std::shared_ptr<AbstractExpression>> aggregates_;
        std::vector<AggregationType> agg_types_;
    };

} // namespace Database