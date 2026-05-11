#pragma once

#include <vector>
#include <memory>
#include "Property.h"

namespace Database
{

    class Group;

    // Type of node within Cascades group expressions
    enum class OpType
    {
        LogicalGet,
        LogicalFilter,
        LogicalJoin,
        PhysicalSeqScan,
        PhysicalIndexScan,
        PhysicalHashJoin,
        PhysicalRadixHashJoin, // Cache friendly hash join
        PhysicalNestedLoopJoin,
        PhysicalSortMergeJoin, // Sort merge join
        PhysicalFilter,
        // ...
        Unknown
    };

    class GroupExpression
    {
    public:
        GroupExpression(OpType type) : type_(type), group_id_(-1) {}

        void AddChild(int child_group_id) { child_groups_.push_back(child_group_id); }

        OpType GetOpType() const { return type_; }
        void SetGroupID(int group_id) { group_id_ = group_id; }
        int GetGroupID() const { return group_id_; }

        const std::vector<int> &GetChildGroupIDs() const { return child_groups_; }

        // A mock method to get cost
        void SetLocalCost(double cost) { local_cost_ = cost; }
        double GetLocalCost() const { return local_cost_; }

    private:
        OpType type_;
        int group_id_;
        std::vector<int> child_groups_;
        double local_cost_{0.0};
    };

} // namespace Database