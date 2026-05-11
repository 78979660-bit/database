#include <iostream>
#include <vector>
#include <cassert>
#include <memory>
#include <chrono>

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
#include "Execution/Executors/SeqScanExecutor.h"
#include "Execution/Expressions/ColumnValueExpression.h"
#include "Execution/Expressions/ConstantValueExpression.h"
#include "Execution/Expressions/ComparisonExpression.h"
#include "Execution/Expressions/ArithmeticExpression.h"
#include "Execution/Expressions/LogicalExpression.h"
#include "Execution/JIT/JitEngine.h"

using namespace Database;

template <typename Func>
double time_it(Func &&func) {
    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

int main() {
    const std::string db_file = "test_benchmark.db";
    DiskManager *disk_manager = new DiskManager(db_file);
    BufferPoolManager *bpm = new BufferPoolManager(5000, disk_manager);
    Catalog *catalog = new Catalog(bpm);
    ExecutionEngine engine(bpm, catalog);
    ExecutorContext exec_ctx(catalog, bpm);

    std::vector<Column> columns;
    columns.emplace_back("id", TypeId::INTEGER);
    columns.emplace_back("age", TypeId::INTEGER);
    columns.emplace_back("salary", TypeId::INTEGER);
    columns.emplace_back("department_id", TypeId::INTEGER);
    columns.emplace_back("bonus", TypeId::INTEGER);
    Schema schema(columns);

    TableMetadata *table_info = catalog->CreateTable(nullptr, "employee", schema);

    constexpr int NUM_ROWS = 2000000;
    std::cout << "Inserting " << NUM_ROWS << " rows..." << std::endl;
    for (int i = 0; i < NUM_ROWS; ++i) {
        std::vector<Value> row;
        row.emplace_back(i);
        row.emplace_back((i % 70) + 20);           
        row.emplace_back((i * 13) % 200000);       
        row.emplace_back(i % 20);                  
        row.emplace_back((i * 7) % 50000);         
        Tuple t(row, &schema);
        RID rid;
        TupleMeta meta;
        meta.insert_txn_id_ = 0;
        table_info->table_->InsertTuple(meta, t, &rid);
    }
    std::cout << "Insert complete." << std::endl;

    auto expr_id = std::make_shared<ColumnValueExpression>(0, 0);
    auto expr_age = std::make_shared<ColumnValueExpression>(0, 1);
    auto expr_sal = std::make_shared<ColumnValueExpression>(0, 2);
    auto expr_dep = std::make_shared<ColumnValueExpression>(0, 3);
    auto expr_bonus = std::make_shared<ColumnValueExpression>(0, 4);

    auto filter_q1 = std::make_shared<ComparisonExpression>(expr_id, std::make_shared<ConstantValueExpression>(Value(500000)), CompType::Equal);

    auto q2_cond1 = std::make_shared<ComparisonExpression>(expr_age, std::make_shared<ConstantValueExpression>(Value(40)), CompType::GreaterThan);
    auto q2_cond2 = std::make_shared<ComparisonExpression>(expr_dep, std::make_shared<ConstantValueExpression>(Value(5)), CompType::Equal);
    auto filter_q2 = std::make_shared<LogicalExpression>(q2_cond1, q2_cond2, LogicType::AND);

    auto q3_math = std::make_shared<ArithmeticExpression>(expr_sal, expr_bonus, ArithType::Add);
    auto filter_q3 = std::make_shared<ComparisonExpression>(q3_math, std::make_shared<ConstantValueExpression>(Value(100000)), CompType::GreaterThan);

    auto sal_div = std::make_shared<ArithmeticExpression>(expr_sal, std::make_shared<ConstantValueExpression>(Value(100)), ArithType::Divide);
    auto bonus_div = std::make_shared<ArithmeticExpression>(expr_bonus, std::make_shared<ConstantValueExpression>(Value(50)), ArithType::Divide);
    auto q4_math = std::make_shared<ArithmeticExpression>(sal_div, bonus_div, ArithType::Add);
    auto q4_cond1 = std::make_shared<ComparisonExpression>(q4_math, std::make_shared<ConstantValueExpression>(Value(1000)), CompType::GreaterThan);
    auto q4_cond2 = std::make_shared<ComparisonExpression>(expr_age, std::make_shared<ConstantValueExpression>(Value(30)), CompType::LessThan);
    auto filter_q4 = std::make_shared<LogicalExpression>(q4_cond1, q4_cond2, LogicType::AND);

    JitEngine jit_engine;
    auto f_q1 = jit_engine.CompileExpression(filter_q1.get());
    auto fb_q1 = jit_engine.CompileBatchExpression(filter_q1.get());

    auto f_q2 = jit_engine.CompileExpression(filter_q2.get());
    auto fb_q2 = jit_engine.CompileBatchExpression(filter_q2.get());

    auto f_q3 = jit_engine.CompileExpression(filter_q3.get());
    auto fb_q3 = jit_engine.CompileBatchExpression(filter_q3.get());

    auto f_q4 = jit_engine.CompileExpression(filter_q4.get());
    auto fb_q4 = jit_engine.CompileBatchExpression(filter_q4.get());

    std::cout << "JIT Compilation done." << std::endl;

    auto test_scenario = [&](const std::string &scenario_name, const AbstractExpression *predicate, JitEngine::CompiledExpressionFunc func, JitEngine::CompiledBatchFunc batch_func) {
        std::cout << "\n--- Scenario: " << scenario_name << " ---" << std::endl;
        size_t grand_match = 0;

        double time_volcano = time_it([&]() {
            ColumnarTable::TableIterator iter = table_info->table_->MakeIterator();
            size_t match = 0;
            while (iter != table_info->table_->MakeEofIterator()) {
                Tuple t = *iter; if (predicate->Evaluate(&t, &schema).GetAsInteger() != 0) {
                    match++;
                }
                ++iter;
            }
            grand_match = match;
        });
        std::cout << "1. Volcano (Row NO JIT): \t" << time_volcano << " ms | Matches: " << grand_match << std::endl;

        double time_jit_scalar = time_it([&]() {
            ColumnarTable::TableIterator iter = table_info->table_->MakeIterator();
            size_t match = 0;
            while (iter != table_info->table_->MakeEofIterator()) {    
                const int32_t *row_data = reinterpret_cast<const int32_t *>((*iter).GetData());
                int32_t res;
                uint8_t is_null;
                func(row_data, &res, &is_null);
                if (!is_null && res != 0) {
                    match++;
                }
                ++iter;
            } 
        });
        std::cout << "2. JIT Scalar (Row WITH JIT): \t" << time_jit_scalar << " ms" << std::endl;

        double time_vectorized = time_it([&]() {
            ColumnarTable::TableIterator iter = table_info->table_->MakeIterator();
            size_t match = 0;
            const size_t batch_size = STANDARD_VECTOR_SIZE;
            Chunk chunk(batch_size);
            for (uint32_t i = 0; i < schema.GetColumnCount(); ++i) {
                chunk.AddVector(std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, batch_size));
            }

            while (iter != table_info->table_->MakeEofIterator()) {    
                size_t row_count = 0;
                chunk.Reset();
                while (iter != table_info->table_->MakeEofIterator() && row_count < batch_size) {
                    for (size_t col_idx = 0; col_idx < schema.GetColumnCount(); ++col_idx) {
                        Tuple t = *iter; Value val = t.GetValue(&schema, col_idx);
                        chunk.GetVector(col_idx)->SetValue(row_count, val);     
                    }
                    row_count++;
                    ++iter;
                }
                chunk.SetCount(row_count);

                std::shared_ptr<Vector> eval_result = std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, batch_size);
                predicate->Evaluate(chunk, eval_result);

                for (size_t i = 0; i < row_count; ++i) {
                    if (eval_result->GetValue(i).GetAsInteger() != 0) {
                        match++;
                    }
                }
            } 
        });
        std::cout << "3. Vectorized (Chunk NO JIT): \t" << time_vectorized << " ms" << std::endl;

        double time_simd = time_it([&]() {
            ColumnarTable::TableIterator iter = table_info->table_->MakeIterator();
            size_t match = 0;
            const size_t batch_size = STANDARD_VECTOR_SIZE;
            Chunk chunk(batch_size);
            for (uint32_t i = 0; i < schema.GetColumnCount(); ++i) {
                chunk.AddVector(std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, batch_size));
            }

            std::vector<const int32_t *> cols(schema.GetColumnCount());
            std::vector<int32_t> results(batch_size);

            while (iter != table_info->table_->MakeEofIterator()) {    
                size_t row_count = 0;
                chunk.Reset();
                while (iter != table_info->table_->MakeEofIterator() && row_count < batch_size) {
                    for (size_t col_idx = 0; col_idx < schema.GetColumnCount(); ++col_idx) {
                        Tuple t = *iter; Value val = t.GetValue(&schema, col_idx);
                        chunk.GetVector(col_idx)->SetValue(row_count, val);     
                    }
                    row_count++;
                    ++iter;
                }
                chunk.SetCount(row_count);

                for (size_t col_idx = 0; col_idx < schema.GetColumnCount(); ++col_idx) {
                    auto flat_vec = std::dynamic_pointer_cast<FlatVector<int32_t>>(chunk.GetVector(col_idx));
                    cols[col_idx] = flat_vec->Data();
                }

                std::vector<uint8_t> nulls(batch_size, 0);
                batch_func(reinterpret_cast<const void**>(cols.data()), results.data(), nulls.data(), row_count);

                for (size_t i = 0; i < row_count; ++i) {
                    if (nulls[i] == 0 && results[i] != 0) {
                        match++;
                    }
                }
            } 
        });
        std::cout << "4. SIMD (Chunk WITH JIT): \t" << time_simd << " ms" << std::endl;

        std::cout << "-> Speedup (Volcano vs SIMD JIT): " << (time_volcano / time_simd) << "x" << std::endl;
    };

    test_scenario("Q1: id = 500000", filter_q1.get(), f_q1, fb_q1);
    test_scenario("Q2: age > 40 AND department_id = 5", filter_q2.get(), f_q2, fb_q2);
    test_scenario("Q3: salary + bonus > 100000", filter_q3.get(), f_q3, fb_q3);
    test_scenario("Q4: (salary / 100) + (bonus / 50) > 1000 AND age < 30", filter_q4.get(), f_q4, fb_q4);

    delete catalog;
    delete bpm;
    delete disk_manager;
    remove(db_file.c_str());

    return 0;
}
