#include "Binder.h"
#include <stdexcept>
#include "../Execution/Expressions/ColumnValueExpression.h"

namespace Database
{

    void Binder::BindStatement(SQLStatement *statement)
    {
        switch (statement->GetType())
        {
        case StatementType::CREATE_TABLE:
            BindCreate(static_cast<CreateStatement *>(statement));
            break;
        case StatementType::INSERT:
            BindInsert(static_cast<InsertStatement *>(statement));
            break;
        case StatementType::SELECT:
            BindSelect(static_cast<SelectStatement *>(statement));
            break;
        default:
            throw std::runtime_error("Binder Error: Unsupported statement type");
        }
    }

    void Binder::BindCreate(CreateStatement *stmt)
    {
        // Check if table already exists
        if (catalog_->GetTable(stmt->table_name_) != nullptr)
        {
            throw std::runtime_error("Binder Error: Table " + stmt->table_name_ + " already exists");
        }
    }

    void Binder::BindInsert(InsertStatement *stmt)
    {
        TableMetadata *table = catalog_->GetTable(stmt->table_name_);
        if (table == nullptr)
        {
            throw std::runtime_error("Binder Error: Table " + stmt->table_name_ + " not found");
        }

        // Basic check: column count match
        uint32_t expected_cols = table->schema_.GetColumnCount();
        for (const auto &row : stmt->values_)
        {
            if (row.size() != expected_cols)
            {
                throw std::runtime_error("Binder Error: Insert column count mismatch");
            }
        }
    }

    void Binder::BindSelect(SelectStatement *stmt)
    {
        TableMetadata *table = catalog_->GetTable(stmt->table_name_);
        if (table == nullptr)
        {
            throw std::runtime_error("Binder Error: Table " + stmt->table_name_ + " not found");
        }

        // Validate select list
        for (const auto &col_name : stmt->select_list_)
        {
            if (col_name != "*" && table->schema_.GetColumnIndex(col_name) == -1)
            {
                throw std::runtime_error("Binder Error: Column " + col_name + " not found in table " + stmt->table_name_);
            }
        }

        // Validate Group By
        for (const auto &col_name : stmt->group_by_list_)
        {
            if (table->schema_.GetColumnIndex(col_name) == -1)
            {
                throw std::runtime_error("Binder Error: Group By column " + col_name + " not found");
            }
        }

        // Validate WHERE clause
        if (stmt->where_filter_)
        {
            BindExpression(stmt->where_filter_.get(), &table->schema_);
        }
    }

    void Binder::BindExpression(AbstractExpression *expr, const Schema *schema)
    {
        if (!expr)
            return;

        if (auto col_expr = dynamic_cast<ColumnValueExpression *>(expr))
        {
            int col_idx = schema->GetColumnIndex(col_expr->GetColumnName());
            if (col_idx == -1)
            {
                throw std::runtime_error("Binder Error: Column " + col_expr->GetColumnName() + " not found");
            }
            col_expr->SetColumnIndex(col_idx); // Resolve string to index!
        }

        for (const auto &child : expr->GetChildren())
        {
            BindExpression(child.get(), schema);
        }
    }

} // namespace Database