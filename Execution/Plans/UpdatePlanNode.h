#pragma once

#include "AbstractPlanNode.h"
#include <string>
#include <unordered_map>
#include "../../Type/Value.h"

namespace Database
{

    class UpdatePlanNode : public AbstractPlanNode
    {
    public:
        UpdatePlanNode(const std::string &table_name, const AbstractPlanNode *child,
                       const std::unordered_map<std::string, Value> &update_attrs)
            : AbstractPlanNode(nullptr, {child}), table_name_(table_name), update_attrs_(update_attrs) {}

        const std::string &GetTableName() const { return table_name_; }

        const std::unordered_map<std::string, Value> &GetUpdateAttrs() const { return update_attrs_; }

        const AbstractPlanNode *GetChildPlan() const
        {
            return GetChildAt(0);
        }

        PlanType GetType() const override { return PlanType::Update; }

    private:
        std::string table_name_;
        std::unordered_map<std::string, Value> update_attrs_;
    };

} // namespace Database
