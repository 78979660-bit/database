#pragma once

#include "AbstractPlanNode.h"
#include <string>
#include "../../Type/Value.h"

namespace Database
{

    class IndexScanPlanNode : public AbstractPlanNode
    {
    public:
        IndexScanPlanNode(const std::shared_ptr<const Schema> &output_schema,
                          std::string index_name,
                          Value pred_value)
            : AbstractPlanNode(output_schema, {}),
              index_name_(std::move(index_name)),
              pred_value_(std::move(pred_value)) {}

        PlanType GetType() const override { return PlanType::IndexScan; }

        const std::string &GetIndexName() const { return index_name_; }
        const Value &GetPredicateValue() const { return pred_value_; }

    private:
        std::string index_name_;
        Value pred_value_; // 等值查询的条件值
    };

} // namespace Database