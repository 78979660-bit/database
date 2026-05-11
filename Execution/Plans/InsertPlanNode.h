#pragma once

#include "AbstractPlanNode.h"
#include <string>

namespace Database
{

    class InsertPlanNode : public AbstractPlanNode
    {
    public:
        InsertPlanNode(const std::shared_ptr<const Schema> &output_schema, const AbstractPlanNode *child, std::string table_name)
            : AbstractPlanNode(output_schema, {child}), table_name_(std::move(table_name)) {}

        PlanType GetType() const override { return PlanType::Insert; }

        std::string GetTableName() const { return table_name_; }

        const AbstractPlanNode *GetChildPlan() const { return GetChildAt(0); }

    private:
        std::string table_name_;
    };

} // namespace Database