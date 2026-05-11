#pragma once

#include "../Parser/SQLStatement.h"
#include "../Catalog/Catalog.h"

namespace Database
{

    class Binder
    {
    public:
        Binder(Catalog *catalog) : catalog_(catalog) {}

        // Validates the statement against the catalog.
        // Throws std::runtime_error if binding fails (e.g., table not found, column not found).
        void BindStatement(SQLStatement *statement);

    private:
        void BindCreate(CreateStatement *stmt);
        void BindInsert(InsertStatement *stmt);
        void BindSelect(SelectStatement *stmt);

        void BindExpression(AbstractExpression *expr, const Schema *schema);

        Catalog *catalog_;
    };

} // namespace Database