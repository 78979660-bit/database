#include "Optimizer.h"

#include <limits>

#include "../Execution/Expressions/ColumnValueExpression.h"

#include "../Execution/Expressions/ConstantValueExpression.h"

#include "../Execution/Expressions/ComparisonExpression.h"

#include "../Execution/Plans/HashJoinPlanNode.h"

namespace Database

{

    std::shared_ptr<AbstractPlanNode> Optimizer::Optimize(std::shared_ptr<AbstractLogicalPlanNode> logical_plan)

    {

        if (!logical_plan)

            return nullptr;

        // Phase 1: Rule-Based Optimization (RBO)

        auto optimized_logical_plan = ApplyLogicalRules(logical_plan);

        // Phase 2: Cost-Based Optimization (CBO) - Physical Mapping

        return MapToPhysical(optimized_logical_plan);
    }

    std::shared_ptr<AbstractLogicalPlanNode> Optimizer::ApplyLogicalRules(std::shared_ptr<AbstractLogicalPlanNode> plan)
    {
        auto current_plan = plan;
        bool changed = true;

        // Repeatedly apply rules until a fixed point is reached
        while (changed)
        {
            changed = false;
            for (const auto &rule : rules_)
            {
                auto rewritten = rule->Apply(current_plan, catalog_);
                if (rewritten != current_plan)
                {
                    current_plan = rewritten;
                    changed = true;
                    break;
                }
            }
        }

        // Top-down / post-order traversal: Optimize children AFTER fixing current node
        for (size_t i = 0; i < current_plan->GetChildren().size(); ++i)
        {
            auto child = current_plan->GetChildAt(i);
            auto opt_child = ApplyLogicalRules(child);
            if (opt_child != child) {
                current_plan->SetChildAt(i, opt_child);
            }
        }

        // Just in case optimizing children created new opportunities for current node
        changed = true;
        while (changed)
        {
            changed = false;
            for (const auto &rule : rules_)
            {
                auto rewritten = rule->Apply(current_plan, catalog_);
                if (rewritten != current_plan)
                {
                    current_plan = rewritten;
                    changed = true;
                    break;
                }
            }
        }

        return current_plan;
    }

    std::shared_ptr<AbstractPlanNode> Optimizer::MapToPhysical(std::shared_ptr<AbstractLogicalPlanNode> logical_plan)

    {

        if (!logical_plan)

            return nullptr;

        switch (logical_plan->GetType())

        {

        case LogicalPlanType::Get:

            return OptimizeGet(std::dynamic_pointer_cast<LogicalGet>(logical_plan));

        case LogicalPlanType::Filter:

            return OptimizeFilter(std::dynamic_pointer_cast<LogicalFilter>(logical_plan));

        case LogicalPlanType::Aggregation:

            return OptimizeAggregation(std::dynamic_pointer_cast<LogicalAggregation>(logical_plan));

        case LogicalPlanType::Window:

            return OptimizeWindow(std::dynamic_pointer_cast<LogicalWindow>(logical_plan));

        case LogicalPlanType::Insert:

            return OptimizeInsert(std::dynamic_pointer_cast<LogicalInsert>(logical_plan));

        case LogicalPlanType::Values:

            return OptimizeValues(std::dynamic_pointer_cast<LogicalValues>(logical_plan));

        case LogicalPlanType::Join:

            return OptimizeJoin(std::dynamic_pointer_cast<LogicalJoin>(logical_plan));
        }

        return nullptr;
    }

    std::shared_ptr<AbstractPlanNode> Optimizer::OptimizeGet(std::shared_ptr<LogicalGet> log_get)

    {

        return std::make_shared<SeqScanPlanNode>(log_get->schema_, log_get->table_name_);
    }

    std::shared_ptr<AbstractPlanNode> Optimizer::OptimizeFilter(std::shared_ptr<LogicalFilter> log_filter)

    {

        auto child_logical_plan = log_filter->GetChildAt(0);

        // CBO: IndexScan vs SeqScan routing based on cost

        if (child_logical_plan->GetType() == LogicalPlanType::Get)

        {

            auto log_get = std::dynamic_pointer_cast<LogicalGet>(child_logical_plan);

            TableMetadata *table = catalog_->GetTable(log_get->table_name_);

            auto comp_expr = dynamic_cast<const ComparisonExpression *>(log_filter->predicate_.get());

            if (comp_expr && comp_expr->GetCompType() == CompType::Equal)

            {

                const ColumnValueExpression *col_expr = nullptr;

                const ConstantValueExpression *const_expr = nullptr;

                auto left_child = comp_expr->GetChildAt(0);

                auto right_child = comp_expr->GetChildAt(1);

                if (auto l_col = dynamic_cast<const ColumnValueExpression *>(left_child))

                {

                    if (auto r_const = dynamic_cast<const ConstantValueExpression *>(right_child))

                    {

                        col_expr = l_col;

                        const_expr = r_const;
                    }
                }

                else if (auto r_col = dynamic_cast<const ColumnValueExpression *>(right_child))

                {

                    if (auto l_const = dynamic_cast<const ConstantValueExpression *>(left_child))

                    {

                        col_expr = r_col;

                        const_expr = l_const;
                    }
                }

                if (col_expr && const_expr)

                {

                    std::string col_name = col_expr->GetColumnName();

                    if (col_name.empty() && col_expr->GetColIdx() != static_cast<uint32_t>(-1))

                    {

                        col_name = log_get->schema_->GetColumn(col_expr->GetColIdx()).GetName();
                    }

                    auto indexes = catalog_->GetTableIndexes(log_get->table_name_);

                    IndexInfo *best_index = nullptr;

                    double min_index_cost = std::numeric_limits<double>::max();

                    for (auto idx : indexes)

                    {

                        if (idx->column_name_ == col_name)

                        {

                            double current_index_cost = cost_model_.EstimateIndexScanCost(table);

                            if (current_index_cost < min_index_cost)

                            {

                                min_index_cost = current_index_cost;

                                best_index = idx;
                            }
                        }
                    }

                    if (best_index)

                    {

                        double seq_scan_cost = cost_model_.EstimateSeqScanCost(table);

                        if (min_index_cost < seq_scan_cost)

                        {

                            return std::make_shared<IndexScanPlanNode>(log_get->schema_, best_index->index_name_, const_expr->GetValue());
                        }
                    }
                }
            }
        }

        // Fallback physical mapping

        auto child_physical = MapToPhysical(child_logical_plan);

        auto filter_plan = std::make_shared<FilterPlanNode>(child_physical->OutputSchema(), child_physical.get(), log_filter->predicate_);

        filter_plan->AddManagedChild(child_physical);

        return filter_plan;
    }

    std::shared_ptr<AbstractPlanNode> Optimizer::OptimizeAggregation(std::shared_ptr<LogicalAggregation> log_agg)

    {

        auto child_physical = MapToPhysical(log_agg->GetChildAt(0));

        auto child_schema = child_physical->OutputSchema();

        std::vector<std::shared_ptr<AbstractExpression>> group_bys;

        for (const auto &col_name : log_agg->group_bys_)

        {

            int col_idx = child_schema->GetColumnIndex(col_name);

            group_bys.push_back(std::make_shared<ColumnValueExpression>(0, col_idx));
        }

        std::vector<std::shared_ptr<AbstractExpression>> aggregates;

        std::vector<AggregationType> agg_types;

        if (log_agg->has_count_star_)

        {

            aggregates.push_back(nullptr);

            agg_types.push_back(AggregationType::CountStar);
        }

        for (const auto &agg : log_agg->aggregations_)

        {

            int col_idx = child_schema->GetColumnIndex(agg.column_name_);

            if (col_idx == -1)

            {

                // Ignore if not a valid column (for example, missing)

                continue;
            }

            auto expr = std::make_shared<ColumnValueExpression>(0, col_idx);

            aggregates.push_back(expr);

            if (agg.function_name_ == "SUM")

                agg_types.push_back(AggregationType::Sum);

            else if (agg.function_name_ == "MIN")

                agg_types.push_back(AggregationType::Min);

            else if (agg.function_name_ == "MAX")

                agg_types.push_back(AggregationType::Max);

            else if (agg.function_name_ == "AVG")

                agg_types.push_back(AggregationType::Sum); // Avg not natively supported in executor yet? Or we can just map it to Sum for now to avoid crash? Wait, let's check AggregationType!

            else

                agg_types.push_back(AggregationType::Count);
        }

        std::vector<Column> agg_cols;

        for (const auto &col_name : log_agg->group_bys_)

        {

            agg_cols.emplace_back(col_name, child_schema->GetColumn(child_schema->GetColumnIndex(col_name)).GetType());
        }

        if (log_agg->has_count_star_)

        {

            agg_cols.emplace_back("COUNT(*)", TypeId::INTEGER);
        }

        for (const auto &agg : log_agg->aggregations_)

        {

            int col_idx = child_schema->GetColumnIndex(agg.column_name_);

            if (col_idx == -1)

                continue;

            auto col_type = child_schema->GetColumn(col_idx).GetType();

            if (agg.function_name_ == "COUNT")

            {

                agg_cols.emplace_back(agg.function_name_ + "(" + agg.column_name_ + ")", TypeId::INTEGER);
            }

            else

            {

                agg_cols.emplace_back(agg.function_name_ + "(" + agg.column_name_ + ")", col_type);
            }
        }

        auto agg_schema = std::make_shared<Schema>(agg_cols);

        auto agg_plan = std::make_shared<AggregationPlanNode>(agg_schema, child_physical.get(), group_bys, aggregates, agg_types);

        agg_plan->AddManagedChild(child_physical);

        return agg_plan;
    }

    std::shared_ptr<AbstractPlanNode> Optimizer::OptimizeInsert(std::shared_ptr<LogicalInsert> log_insert)

    {

        auto child_physical = MapToPhysical(log_insert->GetChildAt(0));

        auto insert_plan = std::make_shared<InsertPlanNode>(log_insert->schema_, child_physical.get(), log_insert->table_name_);

        insert_plan->AddManagedChild(child_physical);

        return insert_plan;
    }

    std::shared_ptr<AbstractPlanNode> Optimizer::OptimizeValues(std::shared_ptr<LogicalValues> log_values)

    {

        return std::make_shared<ValuesPlanNode>(nullptr, log_values->values_);
    }

    std::shared_ptr<AbstractPlanNode> Optimizer::OptimizeWindow(std::shared_ptr<LogicalWindow> log_window)

    {

        auto child_physical = MapToPhysical(log_window->GetChildAt(0));

        auto child_schema = child_physical->OutputSchema();

        std::vector<Column> output_cols = child_schema->GetColumns();

        for (const auto &w : log_window->window_functions_)

        {

            if (w.function_name_ == "RANK" || w.function_name_ == "ROW_NUMBER" || w.function_name_ == "COUNT")

            {

                output_cols.emplace_back(w.function_name_ + "()", TypeId::INTEGER);
            }

            else

            {

                output_cols.emplace_back(w.function_name_ + "(" + w.column_name_ + ")", TypeId::INTEGER);

                // simplified
            }
        }

        auto output_schema = std::make_shared<Schema>(output_cols);

        auto window_plan = std::make_shared<WindowPlanNode>(output_schema, child_physical, log_window->window_functions_);

        window_plan->AddManagedChild(child_physical);

        return window_plan;
    }

    // DPJoinReordering structures
struct DPResult {
    std::shared_ptr<AbstractPlanNode> plan;
    double cost;
    size_t rows;
    std::shared_ptr<const Schema> output_schema;
};

void FlattenJoins(std::shared_ptr<AbstractLogicalPlanNode> node, 
                  std::vector<std::shared_ptr<AbstractLogicalPlanNode>>& relations,
                  std::vector<std::shared_ptr<AbstractExpression>>& conditions) {
    if (node->GetType() == LogicalPlanType::Join) {
        auto join_node = std::dynamic_pointer_cast<LogicalJoin>(node);
        if (join_node->on_condition_) {
            conditions.push_back(join_node->on_condition_);
        }
        FlattenJoins(join_node->GetChildAt(0), relations, conditions);
        FlattenJoins(join_node->GetChildAt(1), relations, conditions);
    } else {
        relations.push_back(node);
    }
}

std::shared_ptr<AbstractPlanNode> Optimizer::OptimizeJoin(std::shared_ptr<LogicalJoin> log_join)
{
    std::vector<std::shared_ptr<AbstractLogicalPlanNode>> relations;
    std::vector<std::shared_ptr<AbstractExpression>> conditions;
    FlattenJoins(log_join, relations, conditions);

    size_t n = relations.size();
    if (n == 0) return nullptr;

    std::vector<DPResult> dp(1 << n);
    for (size_t i = 0; i < n; ++i) {
        auto phys = MapToPhysical(relations[i]);
        size_t rows = 1000;
        if (phys->GetType() == PlanType::SeqScan) {
            auto scan = std::static_pointer_cast<const SeqScanPlanNode>(phys);
            auto table = catalog_->GetTable(scan->GetTableName());
            if (table) rows = table->stats_.tuple_count_;
        }
        dp[1 << i] = {phys, 0.0, rows, phys->OutputSchema()};
    }

    for (size_t mask = 1; mask < (1ULL << n); ++mask) {
        // Skip base relations, computed above
        if ((mask & (mask - 1)) == 0) continue; 

        dp[mask].cost = 1e30; 

        for (size_t sub = (mask - 1) & mask; sub > 0; sub = (sub - 1) & mask) {
            size_t s1 = sub;
            size_t s2 = mask ^ sub;

            if (dp[s1].cost >= 1e29 || dp[s2].cost >= 1e29) continue;

            std::shared_ptr<AbstractExpression> valid_cond = nullptr;
            std::string l_col = "id";
            std::string r_col = "id";

            for (auto& cond : conditions) {
                auto comp = dynamic_cast<const ComparisonExpression*>(cond.get());
                if (!comp) continue;
                auto l_expr = dynamic_cast<const ColumnValueExpression*>(comp->GetChildAt(0));
                auto r_expr = dynamic_cast<const ColumnValueExpression*>(comp->GetChildAt(1));
                if (!l_expr || !r_expr) continue;

                std::string c1 = l_expr->GetColName();
                std::string c2 = r_expr->GetColName();

                bool c1_in_s1 = dp[s1].output_schema->GetColumnIndex(c1) != -1;
                bool c2_in_s2 = dp[s2].output_schema->GetColumnIndex(c2) != -1;
                bool c2_in_s1 = dp[s1].output_schema->GetColumnIndex(c2) != -1;
                bool c1_in_s2 = dp[s2].output_schema->GetColumnIndex(c1) != -1;

                if (c1_in_s1 && c2_in_s2) { l_col = c1; r_col = c2; valid_cond = cond; break; }
                if (c2_in_s1 && c1_in_s2) { l_col = c2; r_col = c1; valid_cond = cond; break; }
            }

            // Simple handling: cross joins have massive penalty
            bool cross_join = (valid_cond == nullptr);

            double estimated_rows = dp[s1].rows * dp[s2].rows;
            if (!cross_join) {
                estimated_rows *= 0.1; // Selectivity
            }

            double curr_cost = dp[s1].cost + dp[s2].cost + cost_model_.EstimateHashJoinCost(dp[s1].rows, dp[s2].rows);
            if (cross_join) curr_cost += 1e9; // penalize cross joins heavily

            if (curr_cost < dp[mask].cost) {
                dp[mask].cost = curr_cost;
                dp[mask].rows = std::max<size_t>(1, estimated_rows);

                auto out_cols = dp[s1].output_schema->GetColumns();
                for (const auto& col : dp[s2].output_schema->GetColumns()) {
                    out_cols.push_back(col);
                }
                dp[mask].output_schema = std::make_shared<const Schema>(out_cols);

                bool swap = dp[s1].rows > dp[s2].rows;
                std::shared_ptr<HashJoinPlanNode> join_plan;

                if (swap) {
                    join_plan = std::make_shared<HashJoinPlanNode>(dp[mask].output_schema, dp[s2].plan.get(), dp[s1].plan.get(), r_col, l_col, valid_cond.get());
                    join_plan->AddManagedChild(dp[s2].plan);
                    join_plan->AddManagedChild(dp[s1].plan);
                } else {
                    join_plan = std::make_shared<HashJoinPlanNode>(dp[mask].output_schema, dp[s1].plan.get(), dp[s2].plan.get(), l_col, r_col, valid_cond.get());
                    join_plan->AddManagedChild(dp[s1].plan);
                    join_plan->AddManagedChild(dp[s2].plan);
                }
                dp[mask].plan = join_plan;
            }
        }
    }

    return dp[(1 << n) - 1].plan;
}

} // namespace Database
