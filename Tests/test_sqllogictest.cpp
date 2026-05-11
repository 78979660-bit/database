#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cassert>
#include <algorithm>
#include <stdexcept>

#include "DiskManager.h"
#include "BufferPoolManager.h"
#include "Catalog/Catalog.h"
#include "Parser/Lexer.h"
#include "Parser/Parser.h"
#include "Planner/Planner.h"
#include "Execution/ExecutionEngine.h"

// =========================================================================
// Real Database Engine Gateway
// =========================================================================
namespace Database
{

    struct QueryResult
    {
        bool is_error = false;
        std::string error_message = "";
        std::vector<std::string> rows;
    };

    class DummyEngineGateway
    {
    public:
        DummyEngineGateway()
        {
            disk_manager_ = new DiskManager("sqllogictest.db");
            bpm_ = new BufferPoolManager(100, disk_manager_);
            catalog_ = new Catalog(bpm_);
            planner_ = new Planner(catalog_);
            execution_engine_ = new ExecutionEngine(bpm_, catalog_);
        }

        ~DummyEngineGateway()
        {
            delete execution_engine_;
            delete planner_;
            delete catalog_;
            delete bpm_;
            delete disk_manager_;
            remove("sqllogictest.db");
        }

        QueryResult ExecuteSQL(const std::string &sql)
        {
            QueryResult res;
            try
            {
                // 1. Lexing & Parsing
                Lexer lexer(sql);
                std::vector<Token> tokens = lexer.Tokenize();
                if (tokens.empty() || tokens[0].type_ == TokenType::END_OF_FILE)
                {
                    return res; // Empty statement
                }

                Parser parser(tokens);
                auto stmt = parser.Parse();

                // 2. Short-circuit CREATE TABLE (since Planner doesn't emit CreateTablePlanNode in some systems)
                if (stmt->GetType() == StatementType::CREATE_TABLE)
                {
                    auto create_stmt = static_cast<CreateStatement *>(stmt.get());
                    Schema schema(create_stmt->columns_);
                    if (!catalog_->CreateTable(nullptr, create_stmt->table_name_, schema))
                    {
                        res.is_error = true;
                        res.error_message = "Table already exists";
                    }
                    return res;
                }

                // 3. Planning & Optimizing
                auto plan = planner_->PlanQuery(stmt.get());
                if (!plan)
                {
                    res.is_error = true;
                    res.error_message = "Failed to plan query";
                    return res;
                }

                // 4. Execution
                auto result_tuples = execution_engine_->Execute(plan.get());

                // 5. Format results
                if (stmt->GetType() == StatementType::SELECT)
                {
                    const Schema *schema = plan->GetOutputSchema();
                    for (const auto &tuple : result_tuples)
                    {
                        for (size_t i = 0; i < schema->GetColumnCount(); ++i)
                        {
                            res.rows.push_back(tuple.GetValue(schema, i).GetAsVarchar());
                        }
                    }
                }
            }
            catch (const std::exception &e)
            {
                res.is_error = true;
                res.error_message = e.what();
            }
            return res;
        }

    private:
        DiskManager *disk_manager_;
        BufferPoolManager *bpm_;
        Catalog *catalog_;
        Planner *planner_;
        ExecutionEngine *execution_engine_;
    };

} // namespace Database

enum class TestState
{
    IDLE,
    PARSING_STATEMENT,
    PARSING_QUERY,
    PARSING_QUERY_RESULTS
};

struct ExpectedQuery
{
    std::string sql;
    std::string type_string;
    std::string sort_mode;
    std::vector<std::string> expected_results;
};

class SqlLogicTestRunner
{
public:
    SqlLogicTestRunner(const std::string &file_path) : file_path_(file_path) {}

    bool Run()
    {
        std::ifstream file(file_path_);
        if (!file.is_open())
        {
            std::cerr << "Failed to open test file: " << file_path_ << "\n";
            return false;
        }

        std::string line;
        TestState state = TestState::IDLE;
        bool expect_error = false;
        std::string current_sql = "";
        ExpectedQuery current_query;
        int line_number = 0;
        int passed_tests = 0;
        int failed_tests = 0;

        auto execute_statement = [&]()
        {
            Database::QueryResult res = engine_.ExecuteSQL(current_sql);
            if (expect_error)
            {
                if (!res.is_error)
                {
                    std::cerr << "Line " << line_number << ": Expected ERROR but got success for SQL:\n"
                              << current_sql << "\n";
                    failed_tests++;
                }
                else
                {
                    passed_tests++;
                }
            }
            else
            {
                if (res.is_error)
                {
                    std::cerr << "Line " << line_number << ": Expected OK but got ERROR (" << res.error_message << ") for SQL:\n"
                              << current_sql << "\n";
                    failed_tests++;
                }
                else
                {
                    passed_tests++;
                }
            }
            current_sql.clear();
            state = TestState::IDLE;
        };

        auto verify_query = [&]()
        {
            Database::QueryResult res = engine_.ExecuteSQL(current_query.sql);
            if (res.is_error)
            {
                std::cerr << "Line " << line_number << ": Query failed with ERROR (" << res.error_message << ")\nSQL:\n"
                          << current_query.sql << "\n";
                failed_tests++;
            }
            else
            {
                // Apply sort mode if needed: 'rowsort' or 'valuesort'
                if (current_query.sort_mode == "rowsort")
                {
                    // Requires row-level assembly to sort. Omitted for skeleton.
                }
                else if (current_query.sort_mode == "valuesort")
                {
                    std::sort(res.rows.begin(), res.rows.end());
                    std::sort(current_query.expected_results.begin(), current_query.expected_results.end());
                }

                if (res.rows != current_query.expected_results)
                {
                    std::cerr << "Line " << line_number << ": Result mismatch for SQL:\n"
                              << current_query.sql << "\n";
                    std::cerr << "Expected " << current_query.expected_results.size() << " values, but got " << res.rows.size() << "\n";
                    failed_tests++;
                }
                else
                {
                    passed_tests++;
                }
            }

            current_query.sql.clear();
            current_query.expected_results.clear();
            state = TestState::IDLE;
        };

        while (std::getline(file, line))
        {
            line_number++;

            // Remove carriage returns if CRLF
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            if (line.empty() || line[0] == '#')
            {
                // Empty lines can act as statement separators
                if (state == TestState::PARSING_STATEMENT && !current_sql.empty())
                {
                    execute_statement();
                }
                else if (state == TestState::PARSING_QUERY_RESULTS)
                {
                    verify_query();
                }
                continue;
            }

            if (state == TestState::IDLE)
            {
                if (line.rfind("statement ok", 0) == 0)
                {
                    expect_error = false;
                    state = TestState::PARSING_STATEMENT;
                }
                else if (line.rfind("statement error", 0) == 0)
                {
                    expect_error = true;
                    state = TestState::PARSING_STATEMENT;
                }
                else if (line.rfind("query", 0) == 0)
                {
                    std::istringstream iss(line);
                    std::string token;
                    iss >> token; // "query"
                    iss >> current_query.type_string;
                    iss >> current_query.sort_mode;
                    if (current_query.sort_mode.empty())
                    {
                        current_query.sort_mode = "nosort";
                    }
                    state = TestState::PARSING_QUERY;
                }
                else if (line == "halt")
                {
                    break;
                }
            }
            else if (state == TestState::PARSING_STATEMENT)
            {
                if (!current_sql.empty())
                    current_sql += " ";
                current_sql += line;
            }
            else if (state == TestState::PARSING_QUERY)
            {
                if (line == "----")
                {
                    state = TestState::PARSING_QUERY_RESULTS;
                }
                else
                {
                    if (!current_query.sql.empty())
                        current_query.sql += " ";
                    current_query.sql += line;
                }
            }
            else if (state == TestState::PARSING_QUERY_RESULTS)
            {
                current_query.expected_results.push_back(line);
            }
        }

        // Flush remaining state
        if (state == TestState::PARSING_STATEMENT && !current_sql.empty())
        {
            execute_statement();
        }
        else if (state == TestState::PARSING_QUERY_RESULTS)
        {
            verify_query();
        }

        std::cout << "--- SqlLogicTest Results ---\n";
        std::cout << "Passed: " << passed_tests << "\n";
        std::cout << "Failed: " << failed_tests << "\n";

        return failed_tests == 0;
    }

private:
    std::string file_path_;
    Database::DummyEngineGateway engine_;
};

int main()
{
    std::cout << "Initializing SqlLogicTest Framework...\n";
    // Usually this path would be dynamic or read from argv
    SqlLogicTestRunner runner("F:/my_c_programme/Database/Tests/SqlLogicTest/sample.test");
    if (runner.Run())
    {
        std::cout << "SUCCESS: All tests passed.\n";
        return 0;
    }
    else
    {
        std::cout << "FAILURE: Some tests failed.\n";
        return 1;
    }
}
