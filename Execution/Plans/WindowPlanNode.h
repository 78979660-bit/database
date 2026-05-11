#pragma once
#include "AbstractPlanNode.h"
#include "../../Parser/SQLStatement.h"
#include <vector>

namespace Database {
class WindowPlanNode : public AbstractPlanNode {
public:
    WindowPlanNode(std::shared_ptr<const Schema> output_schema, std::shared_ptr<AbstractPlanNode> child, std::vector<ParsedWindowFunction> window_functions)
        : AbstractPlanNode(output_schema, {child.get()}), window_functions_(std::move(window_functions)) {}
        
    PlanType GetType() const override { return PlanType::Window; }
    
    std::vector<ParsedWindowFunction> window_functions_;
};
}

