#include <iostream>
#include <vector>
#include <cassert>
#include <memory>
#include <chrono>
#include <atomic>
#include <thread>
#include <future>
#include <iomanip>

#ifdef USE_VTUNE_ITT
#include <ittnotify.h>
#endif

#include "BufferPoolManager.h"
#include "DiskManager.h"
#include "Replacer.h"
#include "SlottedPage.h"
#include "Storage/Table/ColumnarTable.h"

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
#include "Common/ThreadPool.h"
#include "Storage/Table/DictionaryEncoder.h"

using namespace Database;

template <typename Func>
double time_it(Func &&func)
{
    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

int main()
{
    std::cout << "===========================================" << std::endl;
    std::cout << "   Columnar Bit-Packing x JIT Benchmark    " << std::endl;
    std::cout << "        [ 100 MILLION ROWS SCALE ]         " << std::endl;
    std::cout << "===========================================\n"
              << std::endl;

#ifdef USE_VTUNE_ITT
    __itt_domain *domain = __itt_domain_create("DB_Benchmark");
    __itt_string_handle *task_init = __itt_string_handle_create("Data_Generation");
    __itt_string_handle *task_insert = __itt_string_handle_create("Data_Insertion");
    __itt_string_handle *task_scan = __itt_string_handle_create("Parallel_Scan");
    __itt_string_handle *task_jit = __itt_string_handle_create("JIT_Compilation");
#endif

    const std::string db_file = "test_benchmark_extreme_100m.db";
    DiskManager *disk_manager = new DiskManager(db_file);
    BufferPoolManager *bpm = new BufferPoolManager(5000, disk_manager);
    Catalog *catalog = new Catalog(bpm);

    std::vector<Column> columns;
    columns.emplace_back("id", TypeId::INTEGER);
    columns.emplace_back("age", TypeId::INTEGER);
    columns.emplace_back("salary", TypeId::INTEGER);
    columns.emplace_back("department_id", TypeId::INTEGER);
    columns.emplace_back("bonus", TypeId::INTEGER);
    columns.emplace_back("role", TypeId::VARCHAR);
    Schema schema(columns);

    TableMetadata *table_info = catalog->CreateTable(nullptr, "employee_extreme", schema);

    table_info->columnar_storage_->SetColumnEncoding(1, EncodingType::BIT_PACKED, 7); // age 0-127
    table_info->columnar_storage_->SetColumnEncoding(3, EncodingType::BIT_PACKED, 5); // dep 0-31
    table_info->columnar_storage_->SetColumnEncoding(5, EncodingType::BIT_PACKED, 10); // role 0-999

    constexpr int NUM_ROWS = 100000000;
    std::cout << "Inserting " << NUM_ROWS << " rows directly into Catalog's Columnar Storage..." << std::endl;
    std::cout << " -> Applying 7-bit packing on ge column, 5-bit on department_id." << std::endl;

    table_info->columnar_storage_->Reserve(NUM_ROWS);

#ifdef USE_VTUNE_ITT
    __itt_task_begin(domain, __itt_null, __itt_null, task_init);
#endif
    auto start_gen = std::chrono::high_resolution_clock::now();

    auto col0 = std::make_unique<int32_t[]>(NUM_ROWS);
    auto col1 = std::make_unique<int32_t[]>(NUM_ROWS);
    auto col2 = std::make_unique<int32_t[]>(NUM_ROWS);
    auto col3 = std::make_unique<int32_t[]>(NUM_ROWS);
    auto col4 = std::make_unique<int32_t[]>(NUM_ROWS);
    auto col5 = std::make_unique<int32_t[]>(NUM_ROWS);

    std::vector<int32_t> dict_roles;
    for(int i = 0; i < 1000; i++) {
        dict_roles.push_back(StringDictionaryEncoder::GetInstance().Encode("Role_" + std::to_string(i)));
    }

    auto &thread_pool = ThreadPool::Instance();
    unsigned int num_threads = thread_pool.GetThreadCount();
    std::vector<std::future<void>> init_futures;
    size_t chunk_size = NUM_ROWS / num_threads;

    for (unsigned int t = 0; t < num_threads; ++t)
    {
        size_t s_row = t * chunk_size;
        size_t e_row = (t == num_threads - 1) ? NUM_ROWS : s_row + chunk_size;
        init_futures.push_back(thread_pool.Enqueue([&, s_row, e_row]()
                                                   {
            for (size_t i = s_row; i < e_row; ++i) {
                col0[i] = i;
                col1[i] = (i % 70) + 20;
                col2[i] = (i * 13) % 200000;
                col3[i] = i % 20;
                col4[i] = (i * 7) % 50000;
                col5[i] = dict_roles[(i * 17) % 1000];
            } }));
    }
    for (auto &f : init_futures)
        f.wait();
    auto end_gen = std::chrono::high_resolution_clock::now();
#ifdef USE_VTUNE_ITT
    __itt_task_end(domain);
#endif
    std::cout << "Parallel Data Generation load complete. (Took " << std::chrono::duration<double>(end_gen - start_gen).count() << " s)" << std::endl;

#ifdef USE_VTUNE_ITT
    __itt_task_begin(domain, __itt_null, __itt_null, task_insert);
#endif
    auto start_insert = std::chrono::high_resolution_clock::now();
    std::vector<const int32_t *> batch_data = {col0.get(), col1.get(), col2.get(), col3.get(), col4.get(), col5.get()};
    table_info->columnar_storage_->InsertBatch(batch_data, NUM_ROWS);

    auto end_insert = std::chrono::high_resolution_clock::now();
#ifdef USE_VTUNE_ITT
    __itt_task_end(domain);
#endif
    std::cout << "Catalog Columnar Table load complete. (Took " << std::chrono::duration<double>(end_insert - start_insert).count() << " s)\n"
              << std::endl;

    auto expr_id = std::make_shared<ColumnValueExpression>(0, 0);
    auto expr_age = std::make_shared<ColumnValueExpression>(0, 1);
    auto expr_sal = std::make_shared<ColumnValueExpression>(0, 2);
    auto expr_dep = std::make_shared<ColumnValueExpression>(0, 3);
    auto expr_bonus = std::make_shared<ColumnValueExpression>(0, 4);

    auto filter_q1 = std::make_shared<ComparisonExpression>(expr_id, std::make_shared<ConstantValueExpression>(Value(99999999)), CompType::Equal);

    auto expr_role = std::make_shared<ColumnValueExpression>(0, 5);
    int32_t encoded_target_role = StringDictionaryEncoder::GetInstance().Encode("Role_555");
    auto filter_str_q = std::make_shared<ComparisonExpression>(expr_role, std::make_shared<ConstantValueExpression>(Value(encoded_target_role)), CompType::Equal);

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

    auto sal_mul = std::make_shared<ArithmeticExpression>(expr_sal, std::make_shared<ConstantValueExpression>(Value(2)), ArithType::Multiply);
    auto bonus_add = std::make_shared<ArithmeticExpression>(expr_bonus, std::make_shared<ConstantValueExpression>(Value(1)), ArithType::Add);
    auto sal_div_bonus = std::make_shared<ArithmeticExpression>(sal_mul, bonus_add, ArithType::Divide);
    auto q5_cond1 = std::make_shared<ComparisonExpression>(sal_div_bonus, std::make_shared<ConstantValueExpression>(Value(50)), CompType::GreaterThan);
    auto q5_cond2 = std::make_shared<ComparisonExpression>(expr_age, std::make_shared<ConstantValueExpression>(Value(25)), CompType::GreaterThanOrEqual);
    auto q5_cond3 = std::make_shared<ComparisonExpression>(expr_age, std::make_shared<ConstantValueExpression>(Value(45)), CompType::LessThanOrEqual);
    auto q5_cond4 = std::make_shared<ComparisonExpression>(expr_dep, std::make_shared<ConstantValueExpression>(Value(10)), CompType::LessThan);
    auto q5_and1 = std::make_shared<LogicalExpression>(q5_cond1, q5_cond2, LogicType::AND);
    auto q5_and2 = std::make_shared<LogicalExpression>(q5_cond3, q5_cond4, LogicType::AND);
    auto filter_q5 = std::make_shared<LogicalExpression>(q5_and1, q5_and2, LogicType::AND);

#ifdef USE_VTUNE_ITT
    __itt_task_begin(domain, __itt_null, __itt_null, task_jit);
#endif
    JitEngine jit_engine;
    auto fb_q1 = jit_engine.CompileBatchExpression(filter_q1.get());
    auto fb_str = jit_engine.CompileBatchExpression(filter_str_q.get());
    auto fb_q2 = jit_engine.CompileBatchExpression(filter_q2.get());
    auto fb_q3 = jit_engine.CompileBatchExpression(filter_q3.get());
    auto fb_q4 = jit_engine.CompileBatchExpression(filter_q4.get());
    auto fb_q5 = jit_engine.CompileBatchExpression(filter_q5.get());
#ifdef USE_VTUNE_ITT
    __itt_task_end(domain);
#endif
    std::cout << "JIT Compilation done.\n"
              << std::endl;

    auto test_scenario = [&](const std::string &scenario_name, JitEngine::CompiledBatchFunc batch_func, const std::vector<int> &required_cols)
    {
        std::cout << "--- Scan Scenario: " << scenario_name << " ---" << std::endl;

#ifdef USE_VTUNE_ITT
        __itt_task_begin(domain, __itt_null, __itt_null, task_scan);
#endif
        double time_simd = time_it([&]()
                                   {
            size_t total_rows = table_info->columnar_storage_->GetRowCount();   
            constexpr size_t batch_size = 256; // [JIT 内存融合模拟] 极致 L1 Cache Fusion (Micro-Batching)

            auto& thread_pool = ThreadPool::Instance();

            std::atomic<size_t> global_match{0};
            std::vector<std::future<void>> futures;

            // [优化] 切分出海量的小任务，以彻底激活无锁队列的微批次（Micro-Batch）出列优化！
            // 原先: chunk等于 total_rows / 32。 
            // 现在: 按照 CPU 的缓存亲和度，每个任务处理 65536 行。
            size_t chunk_size = 65536; 
            size_t num_chunks = (total_rows + chunk_size - 1) / chunk_size;

            for (size_t t = 0; t < num_chunks; ++t) {
                size_t start_row = t * chunk_size;
                size_t end_row = std::min(total_rows, start_row + chunk_size);

                futures.push_back(thread_pool.Enqueue([&, start_row, end_row, required_cols]() {
                    size_t local_match = 0;
                    std::vector<int32_t> results(batch_size);
                    std::vector<uint8_t> nulls(batch_size, 0);

                    std::vector<std::vector<int32_t>> buffers(5, std::vector<int32_t>(batch_size));

                    size_t current_idx = start_row;
                    while (current_idx < end_row) {
                        size_t current_batch = std::min(batch_size, end_row - current_idx);

                        const void* cols_ptr[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
                        for (int col_idx : required_cols) {
                            cols_ptr[col_idx] = table_info->columnar_storage_->UnpackColumnBatch(col_idx, current_idx, current_batch, buffers[col_idx].data());
                        }

                        batch_func(cols_ptr, results.data(), nulls.data(), current_batch);

                        size_t late_materialize_count = 0;
                        uint32_t sel_vector[batch_size];

                        for (size_t k = 0; k < current_batch; ++k) {
                            sel_vector[late_materialize_count] = current_idx + k;
                            late_materialize_count += ((nulls[k] == 0) & (results[k] != 0));
                        }

                        local_match += late_materialize_count;
                        current_idx += current_batch;
                    }
                    global_match.fetch_add(local_match, std::memory_order_relaxed);
                }));
            }

            for (auto &future : futures) {
                future.wait();
            }
            std::cout << "Match count: " << global_match.load() << std::endl; });
#ifdef USE_VTUNE_ITT
        __itt_task_end(domain);
#endif

        double throughput = (100000000.0 / 1000000.0) / (time_simd / 1000.0);
        std::cout << "PARALLEL SCAN TIME: \t" << time_simd << " ms" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "THROUGHPUT:         \t" << throughput << " Million Rows/sec\n"
                  << std::endl;
    };

    test_scenario("Q1: id = 99999999", fb_q1, {0});
    test_scenario("Q2: age > 40 AND department_id = 5", fb_q2, {1, 3});
    test_scenario("Q3: salary + bonus > 100000", fb_q3, {2, 4});
    test_scenario("Q4: (salary / 100) + (bonus / 50) > 1000 AND age < 30", fb_q4, {1, 2, 4});
    test_scenario("Q5: ((salary * 2)/(bonus + 1)) > 50 AND age >= 25 AND age <= 45 AND dep < 10", fb_q5, {1, 2, 3, 4});

    delete catalog;
    delete bpm;
    delete disk_manager;
    remove(db_file.c_str());

    return 0;
}
