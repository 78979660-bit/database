#pragma once

#include <memory>
#include <string>
#include <vector>
#include "../../Catalog/Schema.h"

namespace Database
{

    enum class PlanType
    {
        SeqScan,
        Insert,
        Values,
        Update,
        Delete,
        Filter,
        Limit,
        Aggregation,
        Window,
        HashJoin,
        NestedLoopJoin,
        IndexScan,
        OrderBy
    };

    class AbstractPlanNode
    {
    public:
        AbstractPlanNode(const std::shared_ptr<const Schema> &schema, std::vector<const AbstractPlanNode *> children = {})
            : output_schema_(schema), children_(std::move(children)) {}

        virtual ~AbstractPlanNode() = default;

        const Schema *GetOutputSchema() const { return output_schema_.get(); }
        std::shared_ptr<const Schema> OutputSchema() const { return output_schema_; }

        const AbstractPlanNode *GetChildAt(size_t index) const
        {
            return children_.at(index);
        }

        const std::vector<const AbstractPlanNode *> &GetChildren() const
        {
            return children_;
        }

        virtual PlanType GetType() const = 0;

        void AddManagedChild(std::shared_ptr<const AbstractPlanNode> child)
        {
            managed_children_.push_back(child);
        }

    private:
        std::shared_ptr<const Schema> output_schema_;
        std::vector<const AbstractPlanNode *> children_;
        std::vector<std::shared_ptr<const AbstractPlanNode>> managed_children_;
    };

} // namespace Database