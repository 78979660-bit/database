#pragma once

#include <memory>
#include <vector>
#include "Memo.h"
#include "Rule.h"
#include "../CostModel.h"

namespace Database
{

    class CascadesOptimizer
    {
    public:
        CascadesOptimizer(CostModel cost_model) : cost_model_(cost_model), memo_(std::make_shared<Memo>())
        {
            RegisterImplementationRules();
            RegisterTransformationRules();
        }

        // Main entry point for the Cascades search
        std::shared_ptr<GroupExpression> Optimize(std::shared_ptr<GroupExpression> root_expr);

    private:
        void RegisterImplementationRules();
        void RegisterTransformationRules();

        void OptimizeGroup(int group_id);
        void OptimizeExpression(std::shared_ptr<GroupExpression> expr);
        void ExploreGroup(int group_id);
        void DeriveLogicalProperties(int group_id);

        double CalculateCost(std::shared_ptr<GroupExpression> expr);

        std::shared_ptr<Memo> memo_;
        CostModel cost_model_;
        std::vector<std::unique_ptr<Rule>> implementation_rules_;
        std::vector<std::unique_ptr<Rule>> transformation_rules_;
    };

} // namespace Database