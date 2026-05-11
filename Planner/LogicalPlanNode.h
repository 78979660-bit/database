#pragma once

#include <vector>
#include <memory>
#include <string>
#include "../Catalog/Schema.h"
#include "../Execution/Expressions/AbstractExpression.h"
#include "../Parser/SQLStatement.h"

namespace Database
{
    enum class LogicalPlanType
    {
        Get,
        Filter,
        Aggregation,
        Window,
        Insert,
        Values,
        Join
    };

    class AbstractLogicalPlanNode
    {
    public:
        AbstractLogicalPlanNode(LogicalPlanType type) : type_(type) {}
        virtual ~AbstractLogicalPlanNode() = default;

        LogicalPlanType GetType() const { return type_; }
        void AddChild(std::shared_ptr<AbstractLogicalPlanNode> child) { children_.push_back(std::move(child)); }
        const std::vector<std::shared_ptr<AbstractLogicalPlanNode>> &GetChildren() const { return children_; }
        std::shared_ptr<AbstractLogicalPlanNode> GetChildAt(size_t index) const { return children_[index]; }
        void SetChildAt(size_t index, std::shared_ptr<AbstractLogicalPlanNode> child) { children_[index] = std::move(child); }
        void ClearChildren() { children_.clear(); }

    protected:
        LogicalPlanType type_;
        std::vector<std::shared_ptr<AbstractLogicalPlanNode>> children_;
    };

    class LogicalGet : public AbstractLogicalPlanNode
    {
    public:
        LogicalGet(std::string table_name, std::shared_ptr<const Schema> schema)
            : AbstractLogicalPlanNode(LogicalPlanType::Get), table_name_(std::move(table_name)), schema_(std::move(schema)) {}

        std::string table_name_;
        std::shared_ptr<const Schema> schema_;
    };

    class LogicalFilter : public AbstractLogicalPlanNode
    {
    public:
        LogicalFilter(std::shared_ptr<AbstractExpression> predicate)
            : AbstractLogicalPlanNode(LogicalPlanType::Filter), predicate_(std::move(predicate)) {}

        std::shared_ptr<AbstractExpression> predicate_;
    };

    class LogicalAggregation : public AbstractLogicalPlanNode
    {
    public:
        LogicalAggregation(std::vector<std::string> group_bys, bool has_count_star, std::vector<ParsedAggregation> aggregations = {})
            : AbstractLogicalPlanNode(LogicalPlanType::Aggregation), group_bys_(std::move(group_bys)), has_count_star_(has_count_star), aggregations_(std::move(aggregations)) {}

        std::vector<std::string> group_bys_;
        bool has_count_star_;
        std::vector<ParsedAggregation> aggregations_;
    };

    class LogicalWindow : public AbstractLogicalPlanNode
    {
    public:
        LogicalWindow(std::vector<ParsedWindowFunction> window_functions)
            : AbstractLogicalPlanNode(LogicalPlanType::Window), window_functions_(std::move(window_functions)) {}

        std::vector<ParsedWindowFunction> window_functions_;
    };

    class LogicalInsert : public AbstractLogicalPlanNode
    {
    public:
        LogicalInsert(std::string table_name, std::shared_ptr<const Schema> schema)
            : AbstractLogicalPlanNode(LogicalPlanType::Insert), table_name_(std::move(table_name)), schema_(std::move(schema)) {}

        std::string table_name_;
        std::shared_ptr<const Schema> schema_;
    };

    class LogicalValues : public AbstractLogicalPlanNode
    {
    public:
        LogicalValues(std::vector<std::vector<Value>> values)
            : AbstractLogicalPlanNode(LogicalPlanType::Values), values_(std::move(values)) {}

        std::vector<std::vector<Value>> values_;
    };

    class LogicalJoin : public AbstractLogicalPlanNode
    {
    public:
        LogicalJoin(JoinType join_type, std::shared_ptr<AbstractExpression> on_condition)
            : AbstractLogicalPlanNode(LogicalPlanType::Join), join_type_(join_type), on_condition_(std::move(on_condition)) {}

        JoinType join_type_;
        std::shared_ptr<AbstractExpression> on_condition_;
    };

} // namespace Database
