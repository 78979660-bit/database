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
#include "Parser/Lexer.h"
#include "Parser/Parser.h"
#include "Binder/Binder.h"
#include "Planner/Planner.h"

using namespace Database;

// Helper to execute SQL from string
void ExecuteSQL(const std::string &sql, Catalog *catalog, ExecutionEngine &engine)
{
    std::cout << "\n[SQL] " << sql << std::endl;

    // 1. Lexer
    std::cout << "-> Lexer" << std::endl;
    Lexer lexer(sql);
    auto tokens = lexer.Tokenize();

    // 2. Parser
    std::cout << "-> Parser" << std::endl;
    Parser parser(tokens);
    auto stmt = parser.Parse();

    // 3. Binder
    std::cout << "-> Binder" << std::endl;
    Binder binder(catalog);
    binder.BindStatement(stmt.get());

    // 4. Planner
    std::cout << "-> Planner" << std::endl;
    Planner planner(catalog);
    auto plan = planner.PlanQuery(stmt.get());

    std::cout << "-> Setup Execution" << std::endl;
    if (stmt->GetType() == StatementType::CREATE_TABLE)
    {
        auto create_stmt = static_cast<CreateStatement *>(stmt.get());
        Schema schema(create_stmt->columns_);
        catalog->CreateTable(nullptr, create_stmt->table_name_, schema);
        std::cout << "Table '" << create_stmt->table_name_ << "' created." << std::endl;
        return;
    }

    if (!plan)
    {
        std::cout << "Failed to generate plan." << std::endl;
        return;
    }

    // 5. Execution
    auto result = engine.Execute(plan.get());
    std::cout << "Results: " << result.size() << " rows." << std::endl;

    if (result.size() > 0 && plan->GetOutputSchema() != nullptr)
    {
        const Schema *schema = plan->GetOutputSchema();
        for (const auto &tuple : result)
        {
            std::cout << "Row: ";
            for (uint32_t i = 0; i < schema->GetColumnCount(); i++)
            {
                Value val = tuple.GetValue(schema, i);
                if (val.GetTypeId() == TypeId::INTEGER)
                {
                    std::cout << val.GetAsInteger();
                }
                else if (val.GetTypeId() == TypeId::VARCHAR)
                {
                    std::cout << val.GetAsVarchar();
                }
                if (i < schema->GetColumnCount() - 1)
                    std::cout << ", ";
            }
            std::cout << std::endl;
        }
    }
}

int main()
{
    const std::string db_file = "test_planner.db";
    remove(db_file.c_str());

    DiskManager *disk_manager = new DiskManager(db_file);
    BufferPoolManager *bpm = new BufferPoolManager(100, disk_manager);
    Catalog *catalog = new Catalog(bpm);
    ExecutionEngine engine(bpm, catalog);

    try
    {
        // DDL
        ExecuteSQL("CREATE TABLE users (id INT, name VARCHAR);", catalog, engine);

        // Insert
        ExecuteSQL("INSERT INTO users VALUES (1, 'Alice');", catalog, engine);
        ExecuteSQL("INSERT INTO users VALUES (2, 'Bob');", catalog, engine);
        ExecuteSQL("INSERT INTO users VALUES (3, 'Alice');", catalog, engine);

        // Simple Select
        ExecuteSQL("SELECT * FROM users;", catalog, engine);

        // Select with Filter
        ExecuteSQL("SELECT * FROM users WHERE id = 2;", catalog, engine);

        // Select with Aggregation
        ExecuteSQL("SELECT name, COUNT(*) FROM users GROUP BY name;", catalog, engine);

        // Error handling test - query non-existent table (should be caught by Binder)
        try
        {
            ExecuteSQL("SELECT * FROM missing_table;", catalog, engine);
        }
        catch (const std::exception &e)
        {
            std::cout << "Caught expected binder error: " << e.what() << std::endl;
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal Error: " << e.what() << std::endl;
    }

    delete catalog;
    delete bpm;
    delete disk_manager;
    remove(db_file.c_str());

    return 0;
}
