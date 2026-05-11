#pragma once

#include "AbstractPlanNode.h"
#include <vector>
#include "../../Type/Value.h"

namespace Database
{

    class ValuesPlanNode : public AbstractPlanNode
    {
    public:
        ValuesPlanNode(const std::shared_ptr<const Schema> &output_schema, std::vector<std::vector<Value>> values)
            : AbstractPlanNode(output_schema, {}), values_(std::move(values)) {}

        const std::vector<std::vector<Value>> &GetValues() const { return values_; }

        virtual PlanType GetType() const override { return PlanType::Values; }

    private:
        std::vector<std::vector<Value>> values_;
    };

} // namespace Database