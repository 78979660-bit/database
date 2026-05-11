#pragma once

#include "AbstractPlanNode.h"
#include <string>

namespace Database
{

    class DeletePlanNode : public AbstractPlanNode
    {
    public:
        DeletePlanNode(const std::string &table_name, const AbstractPlanNode *child)
            : AbstractPlanNode(nullptr, {child}), table_name_(table_name) {}

        const std::string &GetTableName() const { return table_name_; }

        const AbstractPlanNode *GetChildPlan() const
        {
            return GetChildAt(0);
        }

        PlanType GetType() const override { return PlanType::Delete; }

    private:
        std::string table_name_;
    };

} // namespace Database
