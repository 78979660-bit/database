#include "CascadesOptimizer.h"

namespace Database
{

    void CascadesOptimizer::RegisterImplementationRules()
    {
        implementation_rules_.push_back(std::make_unique<LogicalGetToSeqScanRule>());
        implementation_rules_.push_back(std::make_unique<LogicalGetToIndexScanRule>());
        implementation_rules_.push_back(std::make_unique<LogicalJoinToHashJoinRule>());
        implementation_rules_.push_back(std::make_unique<LogicalJoinToNestedLoopJoinRule>());
        implementation_rules_.push_back(std::make_unique<LogicalJoinToSortMergeJoinRule>());
        implementation_rules_.push_back(std::make_unique<LogicalJoinToRadixHashJoinRule>());

        // Add more rules...
    }

    void CascadesOptimizer::RegisterTransformationRules()
    {
        transformation_rules_.push_back(std::make_unique<LogicalJoinCommutativityRule>());
        // Example: Join Associativity, Filter Pushdown, etc.
    }

    std::shared_ptr<GroupExpression> CascadesOptimizer::Optimize(std::shared_ptr<GroupExpression> root_expr)
    {
        int root_group_id = memo_->InsertExpression(root_expr);

        // Standard Cascades top-down approach: Optimize the root group which recursively optimizes children
        OptimizeGroup(root_group_id);

        auto root_group = memo_->GetGroup(root_group_id);
        return root_group->GetBestExpression();
    }

    void CascadesOptimizer::OptimizeGroup(int group_id)
    {
        auto group = memo_->GetGroup(group_id);
        if (group->IsExplored())
        {
            return;
        }

        auto logical_exprs = group->GetLogicalExpressions();
        for (const auto &expr : logical_exprs)
        {
            // First, recursively optimize children groups
            for (int child_group_id : expr->GetChildGroupIDs())
            {
                OptimizeGroup(child_group_id);
            }

            OptimizeExpression(expr);
        }

        // Before evaluation, derive cardinality based on logical structure
        DeriveLogicalProperties(group_id);

        // Evaluate and pick the best physical expression
        double best_cost = 1e9;
        std::shared_ptr<GroupExpression> best_expr = nullptr;

        for (const auto &phys_expr : group->GetPhysicalExpressions())
        {
            double cost = CalculateCost(phys_expr);
            if (cost < best_cost)
            {
                best_cost = cost;
                best_expr = phys_expr;
            }
        }

        group->SetBestExpression(best_expr, best_cost);
        group->SetExplored(true);
    }

    void CascadesOptimizer::DeriveLogicalProperties(int group_id)
    {
        auto group = memo_->GetGroup(group_id);
        if (!group || group->GetLogicalExpressions().empty())
            return;

        auto &props = group->GetLogicalPropertiesMut();
        // Skip if already derived (rough heuristic)
        if (props.estimated_cardinality_ > 0)
            return;

        auto root_expr = group->GetLogicalExpressions().front();
        size_t est_cardinality = 1000; // Base default

        if (root_expr->GetOpType() == OpType::LogicalGet)
        {
            // For a base table, normally we'd fetch from TableStatistics.
            est_cardinality = 10000;
        }
        else if (root_expr->GetOpType() == OpType::LogicalJoin && root_expr->GetChildGroupIDs().size() == 2)
        {
            auto l_group = memo_->GetGroup(root_expr->GetChildGroupIDs()[0]);
            auto r_group = memo_->GetGroup(root_expr->GetChildGroupIDs()[1]);
            size_t left_card = l_group ? l_group->GetLogicalProperties().estimated_cardinality_ : 1000;
            size_t right_card = r_group ? r_group->GetLogicalProperties().estimated_cardinality_ : 1000;

            // Standard generic inner join cardinality estimation:
            // C(A JOIN B) = C(A) * C(B) / max(NDV(A.key), NDV(B.key))
            // Here we assume NDV is roughly 0.1 of cardinality if no HLL stats exist
            size_t ndv_estimate = std::max(static_cast<size_t>(left_card * 0.1), static_cast<size_t>(right_card * 0.1));
            ndv_estimate = std::max<size_t>(1, ndv_estimate);
            est_cardinality = (left_card * right_card) / ndv_estimate;
        }
        else if (root_expr->GetOpType() == OpType::LogicalFilter && root_expr->GetChildGroupIDs().size() >= 1)
        {
            auto c_group = memo_->GetGroup(root_expr->GetChildGroupIDs()[0]);
            size_t child_card = c_group ? c_group->GetLogicalProperties().estimated_cardinality_ : 1000;
            // Assuming EquiDepth Histogram / filtering logic gives 10% selectivity
            est_cardinality = static_cast<size_t>(child_card * 0.1);
        }

        props.estimated_cardinality_ = std::max<size_t>(1, est_cardinality);
    }

    void CascadesOptimizer::ExploreGroup(int group_id)
    {
        // Explore logically (Transformation rules) without optimizing to physical
    }

    void CascadesOptimizer::OptimizeExpression(std::shared_ptr<GroupExpression> expr)
    {
        // Apply applicable implementation rules
        for (const auto &rule : implementation_rules_)
        {
            if (rule->Check(expr))
            {
                auto physical_exprs = rule->Transform(expr);
                for (auto &phys_expr : physical_exprs)
                {
                    // Inherit the same children groups
                    for (int child : expr->GetChildGroupIDs())
                    {
                        if (phys_expr->GetChildGroupIDs().size() < expr->GetChildGroupIDs().size())
                        {
                            phys_expr->AddChild(child);
                        }
                    }
                    memo_->InsertExpression(phys_expr, expr->GetGroupID());
                }
            }
        }

        // Apply transformation rules
        for (const auto &rule : transformation_rules_)
        {
            if (rule->Check(expr))
            {
                auto logical_exprs = rule->Transform(expr);
                for (auto &log_expr : logical_exprs)
                {
                    memo_->InsertExpression(log_expr, expr->GetGroupID());
                    // Should recursively optimize newly generated logical expression
                }
            }
        }
    }

    double CascadesOptimizer::CalculateCost(std::shared_ptr<GroupExpression> expr)
    {
        double cost = 0.0;
        size_t left_rows = 1000;
        size_t right_rows = 1000;

        // Attempt to derive from child groups dynamically
        const auto &children = expr->GetChildGroupIDs();
        if (children.size() > 0)
        {
            auto l_group = memo_->GetGroup(children[0]);
            if (l_group)
            {
                left_rows = std::max<size_t>(1, l_group->GetLogicalProperties().estimated_cardinality_);
            }
        }
        if (children.size() > 1)
        {
            auto r_group = memo_->GetGroup(children[1]);
            if (r_group)
            {
                right_rows = std::max<size_t>(1, r_group->GetLogicalProperties().estimated_cardinality_);
            }
        }

        switch (expr->GetOpType())
        {
        case OpType::PhysicalSeqScan:
            cost = cost_model_.EstimateSeqScanCost(nullptr);
            cost = cost == 0 ? left_rows * 0.2 + left_rows * 1.0 : cost;
            break; // Simple heuristic if table null
        case OpType::PhysicalIndexScan:
            cost = left_rows * 0.05 + 10.0;
            break;
        case OpType::PhysicalHashJoin:
            cost = cost_model_.EstimateHashJoinCost(left_rows, right_rows);
            break;
        case OpType::PhysicalRadixHashJoin:
            cost = cost_model_.EstimateRadixHashJoinCost(left_rows, right_rows);
            break;
        case OpType::PhysicalNestedLoopJoin:
            cost = cost_model_.EstimateNestedLoopJoinCost(left_rows, right_rows);
            break;
        case OpType::PhysicalSortMergeJoin:
            cost = cost_model_.EstimateSortMergeJoinCost(left_rows, right_rows);
            break;
        default:
            cost = 50.0;
            break;
        }

        for (int child_group_id : children)
        {
            auto child_group = memo_->GetGroup(child_group_id);
            if (child_group && child_group->GetBestExpression())
            {
                cost += child_group->GetLowestCost();
            }
        }
        return cost;
    }

} // namespace Database