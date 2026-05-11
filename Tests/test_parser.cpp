#include <iostream>
#include <vector>
#include <cassert>
#include <memory>

#include "BufferPoolManager.h"
#include "DiskManager.h"
#include "Replacer.h"

#include "Type/Value.h"
#include "Catalog/Schema.h"
#include "Catalog/Catalog.h"
#include "Execution/ExecutionEngine.h"
#include "Execution/Plans/SeqScanPlanNode.h"
#include "Execution/Plans/InsertPlanNode.h"
#include "Execution/Plans/ValuesPlanNode.h"
#include "Execution/Plans/FilterPlanNode.h"
#include "Execution/Expressions/ComparisonExpression.h"
#include "Execution/Expressions/ColumnValueExpression.h"
#include "Parser/Lexer.h"
#include "Parser/Parser.h"

using namespace Database;

// Helper to convert SQLStatement to Execution Plans
// In a real system, this is the Job of the Planner/Binder
void ExecuteSQL(const std::string &sql, ExecutionEngine &engine, Catalog *catalog)
{
    std::cout << "\nExecuting SQL: " << sql << std::endl;
    Lexer lexer(sql);
    auto tokens = lexer.Tokenize();

    Parser parser(tokens);
    auto stmt = parser.Parse();

    if (stmt->GetType() == StatementType::CREATE_TABLE)
    {
        auto create_stmt = dynamic_cast<CreateStatement *>(stmt.get());
        Schema schema(create_stmt->columns_);
        catalog->CreateTable(nullptr, create_stmt->table_name_, schema);
        std::cout << "Table '" << create_stmt->table_name_ << "' created." << std::endl;
    }
    else if (stmt->GetType() == StatementType::INSERT)
    {
        auto insert_stmt = dynamic_cast<InsertStatement *>(stmt.get());
        auto table_info = catalog->GetTable(insert_stmt->table_name_);
        if (!table_info)
        {
            std::cerr << "Table not found: " << insert_stmt->table_name_ << std::endl;
            return;
        }

        auto values_plan = std::make_shared<ValuesPlanNode>(
            std::make_shared<Schema>(table_info->schema_),
            insert_stmt->values_);
        auto insert_plan = std::make_shared<InsertPlanNode>(
            std::make_shared<Schema>(table_info->schema_),
            values_plan.get(),
            insert_stmt->table_name_);

        engine.Execute(insert_plan.get());
        std::cout << "Inserted " << insert_stmt->values_.size() << " rows." << std::endl;
    }
    else if (stmt->GetType() == StatementType::SELECT)
    {
        auto select_stmt = dynamic_cast<SelectStatement *>(stmt.get());
        auto table_info = catalog->GetTable(select_stmt->table_name_);
        if (!table_info)
        {
            std::cerr << "Table not found: " << select_stmt->table_name_ << std::endl;
            return;
        }

        auto scan_plan = std::make_shared<SeqScanPlanNode>(
            std::make_shared<Schema>(table_info->schema_),
            select_stmt->table_name_);

        std::shared_ptr<AbstractPlanNode> root_plan = scan_plan;

        if (select_stmt->where_filter_)
        {
            // Specifically resolve ColumnValueExpression for this test
            auto filter_expr = select_stmt->where_filter_;
            auto comp_expr = std::dynamic_pointer_cast<ComparisonExpression>(filter_expr);
            if (comp_expr)
            {
                auto col_expr = dynamic_cast<const ColumnValueExpression *>(comp_expr->GetChildAt(0));
                if (col_expr)
                {
                    int col_idx = table_info->schema_.GetColumnIndex(col_expr->GetColName());
                    // Hack manually setting it for testing (const cast or just know it's a test)
                    const_cast<ColumnValueExpression *>(col_expr)->SetColIdx(col_idx);
                }
            }

            auto filter_plan = std::make_shared<FilterPlanNode>(
                std::make_shared<Schema>(table_info->schema_),
                scan_plan.get(),
                select_stmt->where_filter_);
            root_plan = filter_plan;
        }

        auto result = engine.Execute(root_plan.get());
        std::cout << "Select result (" << result.size() << " rows):" << std::endl;

        // Print header
        const auto &target_columns = select_stmt->select_list_;
        bool select_all = (target_columns.size() == 1 && target_columns[0] == "*");

        std::vector<int> col_indices;
        if (select_all)
        {
            for (uint32_t i = 0; i < table_info->schema_.GetColumnCount(); ++i)
            {
                col_indices.push_back(i);
                std::cout << table_info->schema_.GetColumn(i).GetName() << "\t";
            }
        }
        else
        {
            for (const auto &col_name : target_columns)
            {
                int idx = table_info->schema_.GetColumnIndex(col_name);
                if (idx != -1)
                {
                    col_indices.push_back(idx);
                    std::cout << col_name << "\t";
                }
            }
        }
        std::cout << "\n------------------------" << std::endl;

        // Print rows
        for (const auto &tuple : result)
        {
            for (int idx : col_indices)
            {
                Value val = tuple.GetValue(&table_info->schema_, idx);
                if (val.GetTypeId() == TypeId::INTEGER)
                {
                    std::cout << val.GetAsInteger() << "\t";
                }
                else if (val.GetTypeId() == TypeId::VARCHAR)
                {
                    std::cout << val.GetAsVarchar() << "\t";
                }
            }
            std::cout << std::endl;
        }
    }
}

int main()
{
    const std::string db_file = "test_parser.db";
    DiskManager *disk_manager = new DiskManager(db_file);
    BufferPoolManager *bpm = new BufferPoolManager(100, disk_manager);
    Catalog *catalog = new Catalog(bpm);
    ExecutionEngine engine(bpm, catalog);

    try
    {
        ExecuteSQL("CREATE TABLE users (id INT, name VARCHAR);", engine, catalog);

        ExecuteSQL("INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie');", engine, catalog);

        ExecuteSQL("SELECT * FROM users;", engine, catalog);

        ExecuteSQL("SELECT name FROM users;", engine, catalog);

        ExecuteSQL("SELECT * FROM users WHERE id = 2;", engine, catalog);
        ExecuteSQL("SELECT * FROM users WHERE name = 'Charlie';", engine, catalog);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    // Cleanup
    delete catalog;
    delete bpm;
    delete disk_manager;
    remove(db_file.c_str());

    return 0;
}
