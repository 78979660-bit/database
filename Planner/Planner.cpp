#include "Planner.h"
#include <stdexcept>
#include "../Execution/Expressions/ColumnValueExpression.h"
#include "../Execution/Expressions/ConstantValueExpression.h"
#include "../Execution/Expressions/ComparisonExpression.h"
#include "../Execution/Plans/IndexScanPlanNode.h"

namespace Database
{

    std::shared_ptr<AbstractPlanNode> Planner::PlanQuery(const SQLStatement *statement)
    {
        auto logical_plan = GenerateLogicalPlan(statement);
        if (!logical_plan)
            return nullptr;
        return optimizer_.Optimize(logical_plan);
    }

    std::shared_ptr<AbstractLogicalPlanNode> Planner::GenerateLogicalPlan(const SQLStatement *statement)
    {
        switch (statement->GetType())
        {
        case StatementType::SELECT:
            return GenerateLogicalSelect(static_cast<const SelectStatement *>(statement));
        case StatementType::INSERT:
            return GenerateLogicalInsert(static_cast<const InsertStatement *>(statement));
        default:
            return nullptr;
        }
    }

    std::shared_ptr<AbstractLogicalPlanNode> Planner::GenerateLogicalSelect(const SelectStatement *stmt)
    {
        // 0. Register CTEs into context
        for (const auto &cte : stmt->ctes_)
        {
            cte_context_[cte.cte_name_] = cte.cte_query_;
        }

        std::shared_ptr<AbstractLogicalPlanNode> plan;

        if (stmt->subquery_)
        {
            // 1a. Subquery
            plan = GenerateLogicalSelect(stmt->subquery_.get());
            if (!plan)
                return nullptr;
        }
        else if (cte_context_.find(stmt->table_name_) != cte_context_.end())
        {
            // 1b. CTE source (treat like subquery)
            plan = GenerateLogicalSelect(cte_context_[stmt->table_name_].get());
            if (!plan)
                return nullptr;
        }
        else
        {
            // 1c. Get
            TableMetadata *table = catalog_->GetTable(stmt->table_name_);
            if (!table)
                return nullptr;

            auto schema = std::make_shared<const Schema>(table->schema_);
            plan = std::make_shared<LogicalGet>(stmt->table_name_, schema);
        }
        // 1d. Joins
        for (const auto &join : stmt->joins_)
        {
            std::shared_ptr<AbstractLogicalPlanNode> right_plan;
            TableMetadata *join_table = catalog_->GetTable(join.table_name_);
            if (join_table)
            {
                auto join_schema = std::make_shared<const Schema>(join_table->schema_);
                right_plan = std::make_shared<LogicalGet>(join.table_name_, join_schema);

                auto logical_join = std::make_shared<LogicalJoin>(join.join_type_, join.on_condition_);
                logical_join->AddChild(plan);
                logical_join->AddChild(right_plan);
                plan = logical_join;
            }
        }
        // 2. Filter
        if (stmt->where_filter_)
        {
            auto filter = std::make_shared<LogicalFilter>(stmt->where_filter_);
            filter->AddChild(plan);
            plan = filter;
        }

        // 3. Aggregation
        if (stmt->has_count_star_ || !stmt->group_by_list_.empty() || !stmt->aggregations_.empty())
        {
            auto agg = std::make_shared<LogicalAggregation>(stmt->group_by_list_, stmt->has_count_star_, stmt->aggregations_);
            agg->AddChild(plan);
            plan = agg;
        }

        // 4. Having
        if (stmt->having_filter_)
        {
            auto filter = std::make_shared<LogicalFilter>(stmt->having_filter_);
            filter->AddChild(plan);
            plan = filter;
        }

        // 5. Window Functions
        if (!stmt->window_functions_.empty())
        {
            auto window_node = std::make_shared<LogicalWindow>(stmt->window_functions_);
            window_node->AddChild(plan);
            plan = window_node;
        }

        return plan;
    }

    std::shared_ptr<AbstractLogicalPlanNode> Planner::GenerateLogicalInsert(const InsertStatement *stmt)
    {
        TableMetadata *table = catalog_->GetTable(stmt->table_name_);
        if (!table)
            return nullptr;

        auto schema = std::make_shared<const Schema>(table->schema_);

        auto insert = std::make_shared<LogicalInsert>(stmt->table_name_, schema);
        auto values = std::make_shared<LogicalValues>(stmt->values_);
        insert->AddChild(values);

        return insert;
    }

} // namespace Database
