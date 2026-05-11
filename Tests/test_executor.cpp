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
#include "Execution/Plans/AggregationPlanNode.h"
#include "Execution/Expressions/ColumnValueExpression.h"
#include "Execution/Expressions/ConstantValueExpression.h"
#include "Execution/Expressions/ComparisonExpression.h"

using namespace Database;

int main()
{
    // 1. Setup Environment
    const std::string db_file = "test_executor.db";
    DiskManager *disk_manager = new DiskManager(db_file);
    BufferPoolManager *bpm = new BufferPoolManager(100, disk_manager);
    Catalog *catalog = new Catalog(bpm);
    ExecutionEngine engine(bpm, catalog);

    // 2. Define Schema
    std::vector<Column> columns;
    columns.emplace_back("id", TypeId::INTEGER);
    columns.emplace_back("name", TypeId::VARCHAR);
    Schema schema(columns);

    // 3. Create Table
    TableMetadata *table_info = catalog->CreateTable(nullptr, "users", schema);
    assert(table_info != nullptr);
    std::cout << "Table 'users' created." << std::endl;

    // 4. Create Insert Plan
    // Values to insert: (1, "Alice"), (2, "Bob")
    std::vector<std::vector<Value>> values;
    std::vector<Value> row1;
    row1.emplace_back(1);
    row1.emplace_back("Alice");
    values.push_back(row1);
    values.push_back(row1); // Duplicate for testing GROUP BY

    std::vector<Value> row2;
    row2.emplace_back(2);
    row2.emplace_back("Bob");
    values.push_back(row2);
    values.push_back(row2); // Another duplicate

    std::vector<Value> row3;
    row3.emplace_back(3);
    row3.emplace_back("Charlie");
    values.push_back(row3);

    auto values_plan = std::make_shared<ValuesPlanNode>(std::make_shared<Schema>(schema), values);
    auto insert_plan = std::make_shared<InsertPlanNode>(std::make_shared<Schema>(schema), values_plan.get(), "users");

    // 5. Execute Insert
    std::cout << "Executing Insert..." << std::endl;
    std::vector<Tuple> result = engine.Execute(insert_plan.get());
    std::cout << "Insert finished." << std::endl;
    // Insert executor currently returns empty result set (false)

    // 6. Create Scan Plan
    auto scan_plan = std::make_shared<SeqScanPlanNode>(std::make_shared<Schema>(schema), "users");

    // 6.5 Create Filter Plan (id = 1)
    auto filter_col = std::make_shared<ColumnValueExpression>(0, 0); // tuple_idx = 0, col_idx = 0 (id)
    auto filter_const = std::make_shared<ConstantValueExpression>(Value(1));
    auto filter_expr = std::make_shared<ComparisonExpression>(filter_col, filter_const, CompType::Equal);
    auto filter_plan = std::make_shared<FilterPlanNode>(std::make_shared<Schema>(schema), scan_plan.get(), filter_expr);

    // 7. Execute Scan and Filter
    std::cout << "Executing SeqScan + Filter (id = 1)..." << std::endl;
    result = engine.Execute(filter_plan.get());

    // 8. Verify Results
    std::cout << "Scan Results: " << result.size() << " rows." << std::endl;
    for (const auto &tuple : result)
    {
        // Deserialize and print
        Value id = tuple.GetValue(&schema, 0);
        Value name = tuple.GetValue(&schema, 1);
        std::cout << "Row: " << id.GetAsInteger() << ", " << name.GetAsVarchar() << std::endl;
    }

    // 9. Create Aggregation Plan (GROUP BY name, COUNT(*))
    std::cout << "\nExecuting Aggregation (GROUP BY name, COUNT(*))..." << std::endl;

    // Group By expression: column 1 (name)
    auto group_by_expr = std::make_shared<ColumnValueExpression>(0, 1);
    std::vector<std::shared_ptr<AbstractExpression>> group_bys{group_by_expr};

    // Aggregates: COUNT(*)
    std::vector<std::shared_ptr<AbstractExpression>> aggregates{nullptr}; // No expression needed for CountStar
    std::vector<AggregationType> agg_types{AggregationType::CountStar};

    // Create output schema for Aggregation (name VARCHAR, count INTEGER)
    std::vector<Column> agg_cols;
    agg_cols.emplace_back("name", TypeId::VARCHAR);
    agg_cols.emplace_back("count", TypeId::INTEGER);
    auto agg_schema = std::make_shared<Schema>(agg_cols);

    // Full scan first
    auto full_scan_plan = std::make_shared<SeqScanPlanNode>(std::make_shared<Schema>(schema), "users");

    auto agg_plan = std::make_shared<AggregationPlanNode>(
        agg_schema, full_scan_plan.get(), group_bys, aggregates, agg_types);

    // 10. Execute Aggregation
    result = engine.Execute(agg_plan.get());
    std::cout << "Aggregation Results: " << result.size() << " grouped rows." << std::endl;
    for (const auto &tuple : result)
    {
        // Output schema is: 0: name, 1: count
        Value name = tuple.GetValue(agg_schema.get(), 0);
        Value count = tuple.GetValue(agg_schema.get(), 1);
        std::cout << "Group: " << name.GetAsVarchar() << ", Count: " << count.GetAsInteger() << std::endl;
    }

    // Cleanup
    delete catalog;
    delete bpm;
    delete disk_manager;
    // Remove file if needed
    remove(db_file.c_str());

    return 0;
}
