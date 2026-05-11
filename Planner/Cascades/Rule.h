#pragma once

#include <vector>
#include <memory>
#include "GroupExpression.h"

namespace Database
{

    enum class RuleType
    {
        Transformation, // Logical to Logical (e.g. Join Associativity)
        Implementation  // Logical to Physical (e.g. LogicalJoin->HashJoin)
    };

    class Rule
    {
    public:
        virtual ~Rule() = default;

        virtual RuleType GetType() const = 0;

        // Ensure the current expression matches the root of the rule pattern
        virtual bool Check(std::shared_ptr<GroupExpression> expr) const = 0;

        // Apply the rule. Generates one or more target expressions.
        virtual std::vector<std::shared_ptr<GroupExpression>> Transform(std::shared_ptr<GroupExpression> expr) const = 0;
    };

    // Example rule: LogicalGet -> PhysicalSeqScan
    class LogicalGetToSeqScanRule : public Rule
    {
    public:
        RuleType GetType() const override { return RuleType::Implementation; }

        bool Check(std::shared_ptr<GroupExpression> expr) const override
        {
            return expr->GetOpType() == OpType::LogicalGet;
        }

        std::vector<std::shared_ptr<GroupExpression>> Transform(std::shared_ptr<GroupExpression> expr) const override
        {
            auto physical_expr = std::make_shared<GroupExpression>(OpType::PhysicalSeqScan);
            // In a real system, copy table logical info to physical info
            return {physical_expr};
        }
    };

    // Example rule: LogicalGet -> PhysicalIndexScan
    class LogicalGetToIndexScanRule : public Rule
    {
    public:
        RuleType GetType() const override { return RuleType::Implementation; }

        bool Check(std::shared_ptr<GroupExpression> expr) const override
        {
            return expr->GetOpType() == OpType::LogicalGet;
        }

        std::vector<std::shared_ptr<GroupExpression>> Transform(std::shared_ptr<GroupExpression> expr) const override
        {
            auto physical_expr = std::make_shared<GroupExpression>(OpType::PhysicalIndexScan);
            return {physical_expr};
        }
    };

    // Example rule: LogicalJoin -> HashJoin
    class LogicalJoinToHashJoinRule : public Rule
    {
    public:
        RuleType GetType() const override { return RuleType::Implementation; }

        bool Check(std::shared_ptr<GroupExpression> expr) const override
        {
            return expr->GetOpType() == OpType::LogicalJoin;
        }

        std::vector<std::shared_ptr<GroupExpression>> Transform(std::shared_ptr<GroupExpression> expr) const override
        {
            auto physical_expr = std::make_shared<GroupExpression>(OpType::PhysicalHashJoin);
            for (int child : expr->GetChildGroupIDs())
            {
                physical_expr->AddChild(child);
            }
            return {physical_expr};
        }
    };

    class LogicalJoinToNestedLoopJoinRule : public Rule
    {
    public:
        RuleType GetType() const override { return RuleType::Implementation; }
        bool Check(std::shared_ptr<GroupExpression> expr) const override { return expr->GetOpType() == OpType::LogicalJoin; }
        std::vector<std::shared_ptr<GroupExpression>> Transform(std::shared_ptr<GroupExpression> expr) const override
        {
            auto physical_expr = std::make_shared<GroupExpression>(OpType::PhysicalNestedLoopJoin);
            for (int child : expr->GetChildGroupIDs())
            {
                physical_expr->AddChild(child);
            }
            return {physical_expr};
        }
    };

    class LogicalJoinToSortMergeJoinRule : public Rule
    {
    public:
        RuleType GetType() const override { return RuleType::Implementation; }
        bool Check(std::shared_ptr<GroupExpression> expr) const override { return expr->GetOpType() == OpType::LogicalJoin; }
        std::vector<std::shared_ptr<GroupExpression>> Transform(std::shared_ptr<GroupExpression> expr) const override
        {
            auto physical_expr = std::make_shared<GroupExpression>(OpType::PhysicalSortMergeJoin);
            for (int child : expr->GetChildGroupIDs())
            {
                physical_expr->AddChild(child);
            }
            return {physical_expr};
        }
    };

    class LogicalJoinToRadixHashJoinRule : public Rule
    {
    public:
        RuleType GetType() const override { return RuleType::Implementation; }
        bool Check(std::shared_ptr<GroupExpression> expr) const override { return expr->GetOpType() == OpType::LogicalJoin; }
        std::vector<std::shared_ptr<GroupExpression>> Transform(std::shared_ptr<GroupExpression> expr) const override
        {
            auto physical_expr = std::make_shared<GroupExpression>(OpType::PhysicalRadixHashJoin);
            for (int child : expr->GetChildGroupIDs())
            {
                physical_expr->AddChild(child);
            }
            return {physical_expr};
        }
    };

    // LogicalJoin Commutativity Rule: A JOIN B -> B JOIN A
    class LogicalJoinCommutativityRule : public Rule
    {
    public:
        RuleType GetType() const override { return RuleType::Transformation; }
        bool Check(std::shared_ptr<GroupExpression> expr) const override
        {
            return expr->GetOpType() == OpType::LogicalJoin && expr->GetChildGroupIDs().size() == 2;
        }
        std::vector<std::shared_ptr<GroupExpression>> Transform(std::shared_ptr<GroupExpression> expr) const override
        {
            auto commutated_expr = std::make_shared<GroupExpression>(OpType::LogicalJoin);
            const auto &children = expr->GetChildGroupIDs();
            commutated_expr->AddChild(children[1]);
            commutated_expr->AddChild(children[0]);
            return {commutated_expr};
        }
    };

} // namespace Database
