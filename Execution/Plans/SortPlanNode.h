#pragma once

#include "AbstractPlanNode.h"
#include <vector>
#include <string>

namespace Database
{

    class SortPlanNode : public AbstractPlanNode
    {
    public:
        SortPlanNode(Schema *output_schema, const AbstractPlanNode *child, const std::vector<std::string> &order_bys, const std::vector<bool> &is_asc)
            : AbstractPlanNode(output_schema, {child}), order_bys_(order_bys), is_asc_(is_asc) {}

        PlanType GetType() const override { return PlanType::OrderBy; }

        const AbstractPlanNode *GetChildPlan() const
        {
            return GetChildAt(0);
        }

        const std::vector<std::string> &GetOrderBys() const { return order_bys_; }
        const std::vector<bool> &GetIsAscs() const { return is_asc_; }

    private:
        std::vector<std::string> order_bys_;
        std::vector<bool> is_asc_;
    };

} // namespace Database