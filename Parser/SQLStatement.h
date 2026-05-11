#pragma once

#include <string>
#include <vector>
#include <memory>
#include "../Type/Value.h"
#include "../Catalog/Schema.h"
#include "../Execution/Expressions/AbstractExpression.h"

#undef DELETE
#undef UPDATE
#undef INSERT

namespace Database
{

    enum class StatementType
    {
        CREATE_TABLE,
        INSERT,
        SELECT,
        UPDATE,
        DELETE,
        DROP_TABLE,
        CREATE_INDEX,
        BEGIN_TRANSACTION,
        COMMIT_TRANSACTION,
        ROLLBACK_TRANSACTION,
        INVALID
    };

    class SQLStatement
    {
    public:
        virtual ~SQLStatement() = default;
        virtual StatementType GetType() const = 0;
    };

    // BEGIN TRANSACTION
    class BeginStatement : public SQLStatement
    {
    public:
        StatementType GetType() const override { return StatementType::BEGIN_TRANSACTION; }
    };

    // COMMIT
    class CommitStatement : public SQLStatement
    {
    public:
        StatementType GetType() const override { return StatementType::COMMIT_TRANSACTION; }
    };

    // ROLLBACK
    class RollbackStatement : public SQLStatement
    {
    public:
        StatementType GetType() const override { return StatementType::ROLLBACK_TRANSACTION; }
    };

    // DROP TABLE
    class DropTableStatement : public SQLStatement
    {
    public:
        DropTableStatement(std::string table_name) : table_name_(std::move(table_name)) {}
        StatementType GetType() const override { return StatementType::DROP_TABLE; }
        std::string table_name_;
    };

    // CREATE INDEX
    class CreateIndexStatement : public SQLStatement
    {
    public:
        CreateIndexStatement(std::string index_name, std::string table_name, std::vector<std::string> index_keys, bool unique)
            : index_name_(std::move(index_name)), table_name_(std::move(table_name)), index_keys_(std::move(index_keys)), unique_(unique) {}
        StatementType GetType() const override { return StatementType::CREATE_INDEX; }
        std::string index_name_;
        std::string table_name_;
        std::vector<std::string> index_keys_;
        bool unique_;
    };

    // DELETE
    class DeleteStatement : public SQLStatement
    {
    public:
        DeleteStatement(std::string table_name, std::shared_ptr<AbstractExpression> where_filter = nullptr)
            : table_name_(std::move(table_name)), where_filter_(std::move(where_filter)) {}
        StatementType GetType() const override { return StatementType::DELETE; }
        std::string table_name_;
        std::shared_ptr<AbstractExpression> where_filter_;
    };

    // UPDATE
    class UpdateStatement : public SQLStatement
    {
    public:
        UpdateStatement(std::string table_name, std::vector<std::pair<std::string, std::shared_ptr<AbstractExpression>>> updates, std::shared_ptr<AbstractExpression> where_filter = nullptr)
            : table_name_(std::move(table_name)), updates_(std::move(updates)), where_filter_(std::move(where_filter)) {}
        StatementType GetType() const override { return StatementType::UPDATE; }
        std::string table_name_;
        std::vector<std::pair<std::string, std::shared_ptr<AbstractExpression>>> updates_; // Column -> Expression
        std::shared_ptr<AbstractExpression> where_filter_;
    };

    // CREATE TABLE
    class CreateStatement : public SQLStatement
    {
    public:
        CreateStatement(std::string table_name, std::vector<Column> columns)
            : table_name_(std::move(table_name)), columns_(std::move(columns)) {}

        StatementType GetType() const override { return StatementType::CREATE_TABLE; }

        std::string table_name_;
        std::vector<Column> columns_;
    };

    // INSERT INTO
    class InsertStatement : public SQLStatement
    {
    public:
        InsertStatement(std::string table_name, std::vector<std::vector<Value>> values)
            : table_name_(std::move(table_name)), values_(std::move(values)) {}

        StatementType GetType() const override { return StatementType::INSERT; }

        std::string table_name_;
        std::vector<std::vector<Value>> values_;
    };

    struct ParsedAggregation
    {
        std::string function_name_;
        std::string column_name_;
    };

    class SelectStatement;

    struct CommonTableExpression
    {
        std::string cte_name_;
        std::shared_ptr<SelectStatement> cte_query_;
    };

    struct ParsedWindowFunction
    {
        std::string function_name_; // e.g. ROW_NUMBER, RANK, SUM
        std::string column_name_;   // e.g. for SUM(col)
        std::vector<std::string> partition_by_;
        std::vector<std::pair<std::string, bool>> order_by_; // column_name, is_asc
    };

    enum class JoinType
    {
        INNER,
        LEFT,
        RIGHT,
        OUTER,
        INVALID
    };

    struct JoinClause
    {
        JoinType join_type_;
        std::string table_name_;
        std::string table_alias_;
        std::shared_ptr<AbstractExpression> on_condition_;
    };

    // SELECT
    class SelectStatement : public SQLStatement
    {
    public:
        SelectStatement(std::string table_name, std::vector<std::string> select_list,
                        std::shared_ptr<AbstractExpression> where_filter = nullptr,
                        std::vector<std::string> group_by_list = {},
                        bool has_count_star = false,
                        std::vector<ParsedAggregation> aggregations = {},
                        std::shared_ptr<AbstractExpression> having_filter = nullptr,
                        std::shared_ptr<SelectStatement> subquery = nullptr,
                        std::string table_alias = "",
                        std::vector<CommonTableExpression> ctes = {},
                        std::vector<ParsedWindowFunction> window_functions = {},
                        std::vector<JoinClause> joins = {})
            : table_name_(std::move(table_name)), select_list_(std::move(select_list)),
              where_filter_(std::move(where_filter)), group_by_list_(std::move(group_by_list)),
              has_count_star_(has_count_star), aggregations_(std::move(aggregations)),
              having_filter_(std::move(having_filter)), subquery_(std::move(subquery)), table_alias_(std::move(table_alias)), ctes_(std::move(ctes)), window_functions_(std::move(window_functions)), joins_(std::move(joins)) {}

        StatementType GetType() const override { return StatementType::SELECT; }

        std::string table_name_;
        std::vector<std::string> select_list_; // Use "*" for all columns
        std::shared_ptr<AbstractExpression> where_filter_;
        std::vector<std::string> group_by_list_;
        bool has_count_star_;
        std::vector<ParsedAggregation> aggregations_;
        std::shared_ptr<AbstractExpression> having_filter_;
        std::shared_ptr<SelectStatement> subquery_;
        std::string table_alias_;
        std::vector<CommonTableExpression> ctes_;
        std::vector<ParsedWindowFunction> window_functions_;
        std::vector<JoinClause> joins_;
    };

} // namespace Database