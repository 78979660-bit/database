#pragma once

#include <memory>
#include <vector>
#include "LogicalPlanNode.h"
#include "../Execution/Plans/AbstractPlanNode.h"
#include "../Execution/Plans/SeqScanPlanNode.h"
#include "../Execution/Plans/InsertPlanNode.h"
#include "../Execution/Plans/ValuesPlanNode.h"
#include "../Execution/Plans/FilterPlanNode.h"
#include "../Execution/Plans/AggregationPlanNode.h"
#include "../Execution/Plans/WindowPlanNode.h"
#include "../Execution/Plans/IndexScanPlanNode.h"
#include "../Catalog/Catalog.h"
#include "CostModel.h"
#include "OptimizerRule.h"
#include "RewriteRules.h"
#include "RewriteRules.h"

namespace Database
{

    class Optimizer
    {
    public:
        Optimizer(Catalog *catalog) : catalog_(catalog)
        {
            // Register rules here (e.g. FilterPushDownRule)
            // rules_.push_back(std::make_unique<FilterPushDownRule>());
        }

        // Framework entry point: Applies rules then maps to physical implementation
        std::shared_ptr<AbstractPlanNode> Optimize(std::shared_ptr<AbstractLogicalPlanNode> logical_plan);

    private:
        // Rule-based logical rewriting (RBO phase)
        std::shared_ptr<AbstractLogicalPlanNode> ApplyLogicalRules(std::shared_ptr<AbstractLogicalPlanNode> plan);

        // Cost-based physical mapping (CBO phase)
        std::shared_ptr<AbstractPlanNode> MapToPhysical(std::shared_ptr<AbstractLogicalPlanNode> logical_plan);
        std::shared_ptr<AbstractPlanNode> OptimizeGet(std::shared_ptr<LogicalGet> log_get);
        std::shared_ptr<AbstractPlanNode> OptimizeFilter(std::shared_ptr<LogicalFilter> log_filter);
        std::shared_ptr<AbstractPlanNode> OptimizeAggregation(std::shared_ptr<LogicalAggregation> log_agg);
        std::shared_ptr<AbstractPlanNode> OptimizeWindow(std::shared_ptr<LogicalWindow> log_window);
        std::shared_ptr<AbstractPlanNode> OptimizeInsert(std::shared_ptr<LogicalInsert> log_insert);
        std::shared_ptr<AbstractPlanNode> OptimizeValues(std::shared_ptr<LogicalValues> log_values);
        std::shared_ptr<AbstractPlanNode> OptimizeJoin(std::shared_ptr<LogicalJoin> log_join);

        Catalog *catalog_;
        CostModel cost_model_;
        std::vector<std::unique_ptr<OptimizerRule>> rules_;
    };

} // namespace Database
