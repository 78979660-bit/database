#pragma once

#include "AbstractPlanNode.h"
#include <string>
#include <vector>

namespace Database
{
    class AbstractExpression;

    class SeqScanPlanNode : public AbstractPlanNode
    {
    public:
        SeqScanPlanNode(const std::shared_ptr<const Schema> &output_schema, std::string table_name,
                        const AbstractExpression *predicate = nullptr,
                        std::vector<uint32_t> predicate_column_ids = {})
            : AbstractPlanNode(output_schema, {}), table_name_(std::move(table_name)),
              predicate_(predicate), predicate_column_ids_(std::move(predicate_column_ids)) {}

        PlanType GetType() const override { return PlanType::SeqScan; }

        std::string GetTableName() const { return table_name_; }

        const AbstractExpression *GetPredicate() const { return predicate_; }

        const std::vector<uint32_t> &GetPredicateColumnIds() const { return predicate_column_ids_; }

    private:
        std::string table_name_;
        const AbstractExpression *predicate_{nullptr};
        std::vector<uint32_t> predicate_column_ids_;
    };

} // namespace Database