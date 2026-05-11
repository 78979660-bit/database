#pragma once

#include <memory>
#include "LogicalPlanNode.h"
#include "../Catalog/Catalog.h"

namespace Database
{
    // Base class for all optimizer rewrite rules.
    class OptimizerRule
    {
    public:
        virtual ~OptimizerRule() = default;

        // Apply rule to the logical plan node. Returns the rewritten plan,
        // or the original plan if no rewrite is applicable.
        virtual std::shared_ptr<AbstractLogicalPlanNode> Apply(
            std::shared_ptr<AbstractLogicalPlanNode> plan,
            Catalog *catalog) const = 0;

        virtual std::string GetName() const = 0;
    };

} // namespace Database
