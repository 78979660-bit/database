#pragma once

#include <vector>
#include <memory>
#include "Group.h"

namespace Database
{

    class Memo
    {
    public:
        Memo() = default;

        int AddNewGroup()
        {
            int next_id = groups_.size();
            groups_.push_back(std::make_shared<Group>(next_id));
            return next_id;
        }

        std::shared_ptr<Group> GetGroup(int id) const
        {
            if (id < 0 || id >= static_cast<int>(groups_.size()))
            {
                return nullptr;
            }
            return groups_[id];
        }

        int InsertExpression(std::shared_ptr<GroupExpression> expr, int target_group_id = -1)
        {
            // For a real implementation, duplicate detection is required here based on Expression signatures.
            // Simplified:
            if (target_group_id == -1)
            {
                target_group_id = AddNewGroup();
            }

            auto group = GetGroup(target_group_id);
            if (expr->GetOpType() == OpType::PhysicalSeqScan ||
                expr->GetOpType() == OpType::PhysicalIndexScan ||
                expr->GetOpType() == OpType::PhysicalHashJoin ||
                expr->GetOpType() == OpType::PhysicalFilter ||
                expr->GetOpType() == OpType::PhysicalNestedLoopJoin)
            {
                group->AddPhysicalExpression(expr);
            }
            else
            {
                group->AddExpression(expr);
            }

            return target_group_id;
        }

        const std::vector<std::shared_ptr<Group>> &GetGroups() const { return groups_; }

    private:
        std::vector<std::shared_ptr<Group>> groups_;
    };

} // namespace Database