#pragma once

#include <vector>
#include <memory>
#include <unordered_map>
#include "GroupExpression.h"
#include "../../Catalog/TableStatistics.h"

namespace Database
{

    struct LogicalProperties
    {
        size_t estimated_cardinality_{0};
        // A placeholder for tracking HLL NDV estimates across joined paths
        std::map<std::string, std::shared_ptr<HyperLogLog>> column_ndv_hll_;
    };

    class Group
    {
    public:
        Group(int id) : id_(id), explored_(false) {}

        void AddExpression(std::shared_ptr<GroupExpression> expr)
        {
            expr->SetGroupID(id_);
            logical_expressions_.push_back(expr);
        }

        void AddPhysicalExpression(std::shared_ptr<GroupExpression> expr)
        {
            expr->SetGroupID(id_);
            physical_expressions_.push_back(expr);
        }

        const std::vector<std::shared_ptr<GroupExpression>> &GetLogicalExpressions() const { return logical_expressions_; }
        const std::vector<std::shared_ptr<GroupExpression>> &GetPhysicalExpressions() const { return physical_expressions_; }

        int GetID() const { return id_; }

        void SetExplored(bool explored) { explored_ = explored; }
        bool IsExplored() const { return explored_; }

        // Mocks for tracking the best physical expression per physical property
        void SetBestExpression(std::shared_ptr<GroupExpression> expr, double cost)
        {
            best_expression_ = expr;
            lowest_cost_ = cost;
        }

        std::shared_ptr<GroupExpression> GetBestExpression() const { return best_expression_; }
        double GetLowestCost() const { return lowest_cost_; }

        // Logical properties logic
        void SetLogicalProperties(const LogicalProperties &props) { logical_properties_ = props; }
        const LogicalProperties &GetLogicalProperties() const { return logical_properties_; }
        LogicalProperties &GetLogicalPropertiesMut() { return logical_properties_; }

    private:
        int id_;
        bool explored_;
        std::vector<std::shared_ptr<GroupExpression>> logical_expressions_;
        std::vector<std::shared_ptr<GroupExpression>> physical_expressions_;

        LogicalProperties logical_properties_;

        // Simulating the best plan without specific physical properties (for simplicity)
        std::shared_ptr<GroupExpression> best_expression_{nullptr};
        double lowest_cost_{1e9}; // large default
    };

} // namespace Database