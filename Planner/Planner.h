#pragma once

#include <memory>
#include <unordered_map>
#include "../Parser/SQLStatement.h"
#include "../Catalog/Catalog.h"
#include "../Execution/Plans/AbstractPlanNode.h"
#include "../Execution/Plans/SeqScanPlanNode.h"
#include "../Execution/Plans/InsertPlanNode.h"
#include "../Execution/Plans/ValuesPlanNode.h"
#include "../Execution/Plans/FilterPlanNode.h"
#include "../Execution/Plans/AggregationPlanNode.h"
#include "LogicalPlanNode.h"
#include "Optimizer.h"

namespace Database
{

    class Planner
    {
    public:
        Planner(Catalog *catalog) : catalog_(catalog), optimizer_(catalog) {}

        // Generates a logical/physical plan from a bound SQLStatement.
        std::shared_ptr<AbstractPlanNode> PlanQuery(const SQLStatement *statement);

    private:
        // Step 1: SQL to Logical Plan
        std::shared_ptr<AbstractLogicalPlanNode> GenerateLogicalPlan(const SQLStatement *statement);
        std::shared_ptr<AbstractLogicalPlanNode> GenerateLogicalSelect(const SelectStatement *stmt);
        std::shared_ptr<AbstractLogicalPlanNode> GenerateLogicalInsert(const InsertStatement *stmt);

        Catalog *catalog_;
        Optimizer optimizer_;

        // CTE Context for nested planning
        std::unordered_map<std::string, std::shared_ptr<SelectStatement>> cte_context_;
    };

} // namespace Database