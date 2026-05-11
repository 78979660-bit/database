#include <iostream>
#include <vector>
#include <cassert>
#include <memory>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <future>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <immintrin.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef USE_VTUNE_ITT
#include <ittnotify.h>
#endif

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

static inline size_t CountNonZeroInt32(const int32_t *values, size_t count) {
    size_t matches = 0;
    size_t i = 0;
#if defined(__AVX2__)
    const __m256i zero = _mm256_setzero_si256();
    for (; i + 8 <= count; i += 8) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(values + i));
        __m256i eq_zero = _mm256_cmpeq_epi32(v, zero);
        uint32_t zero_mask = static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(eq_zero)));
        matches += 8u - static_cast<uint32_t>(__builtin_popcount(zero_mask));
    }
#endif
    for (; i < count; ++i) {
        matches += values[i] != 0;
    }
    return matches;
}

class BenchmarkThreadPool {
public:
    explicit BenchmarkThreadPool(size_t worker_count, bool pin_pcores) : pin_pcores_(pin_pcores), results_(worker_count, 0) {
        workers_.reserve(worker_count);
        for (size_t worker = 0; worker < worker_count; ++worker) {
            workers_.emplace_back([this, worker]() { WorkerLoop(worker); });
        }
    }

    ~BenchmarkThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            generation_++;
        }
        start_cv_.notify_all();
        for (auto &thread : workers_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    template <typename Func>
    size_t Run(Func &&func) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            work_ = std::forward<Func>(func);
            remaining_ = workers_.size();
            std::fill(results_.begin(), results_.end(), 0);
            generation_++;
        }

        start_cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this]() { return remaining_ == 0; });

        size_t total = 0;
        for (size_t value : results_) {
            total += value;
        }
        work_ = nullptr;
        return total;
    }

    size_t Size() const { return workers_.size(); }

private:
    void PinWorker(size_t worker) {
#ifdef _WIN32
        if (!pin_pcores_) {
            return;
        }

        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) || process_mask == 0) {
            return;
        }

        // Common Intel hybrid layout exposes P-core SMT threads first.
        const size_t pcore_threads = std::min<size_t>(16, sizeof(DWORD_PTR) * 8);
        DWORD_PTR preferred = DWORD_PTR{1} << (worker % pcore_threads);
        DWORD_PTR mask = preferred & process_mask;
        if (mask == 0) {
            mask = process_mask;
        }
        SetThreadAffinityMask(GetCurrentThread(), mask);
#else
        (void)worker;
#endif
    }

    void WorkerLoop(size_t worker) {
        PinWorker(worker);
        size_t seen_generation = 0;
        while (true) {
            std::function<size_t(size_t)> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                start_cv_.wait(lock, [this, seen_generation]() {
                    return stop_ || generation_ != seen_generation;
                });
                if (stop_) {
                    return;
                }
                seen_generation = generation_;
                work = work_;
            }

            results_[worker] = work(worker);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (--remaining_ == 0) {
                    done_cv_.notify_one();
                }
            }
        }
    }

    std::vector<std::thread> workers_;
    std::vector<size_t> results_;
    std::mutex mutex_;
    std::condition_variable start_cv_;
    std::condition_variable done_cv_;
    std::function<size_t(size_t)> work_;
    size_t remaining_ = 0;
    size_t generation_ = 0;
    bool pin_pcores_ = false;
    bool stop_ = false;
};

struct ScanWorkerContext {
    std::vector<const int32_t *> cols;
    std::vector<int32_t> results;
    std::vector<uint8_t> nulls;
    std::vector<std::vector<int32_t>> buffers;

    ScanWorkerContext(size_t column_count, size_t batch_size)
        : cols(column_count),
          results(batch_size),
          nulls(batch_size, 0),
          buffers(column_count, std::vector<int32_t>(batch_size)) {}
};

static inline void VtunePause(bool enabled) {
#ifdef USE_VTUNE_ITT
    if (enabled) {
        __itt_pause();
    }
#else
    (void)enabled;
#endif
}

static inline void VtuneResume(bool enabled) {
#ifdef USE_VTUNE_ITT
    if (enabled) {
        __itt_resume();
    }
#else
    (void)enabled;
#endif
}

int main(int argc, char **argv) {
    bool simd_only = false;
    bool parallel_only = false;
    bool pin_pcores = false;
    size_t repeat_count = 1;
    unsigned requested_workers = 0;
    for (int arg_idx = 1; arg_idx < argc; ++arg_idx) {
        std::string arg = argv[arg_idx];
        if (arg == "--simd-only") {
            simd_only = true;
        } else if (arg == "--parallel-only") {
            parallel_only = true;
            simd_only = true;
        } else if (arg == "--pin-pcores") {
            pin_pcores = true;
        } else if (arg == "--workers" && arg_idx + 1 < argc) {
            requested_workers = static_cast<unsigned>(std::max<unsigned long long>(1, std::stoull(argv[++arg_idx])));
        } else if (arg == "--repeat" && arg_idx + 1 < argc) {
            repeat_count = std::max<size_t>(1, static_cast<size_t>(std::stoull(argv[++arg_idx])));
        }
    }
#ifdef _WIN32
    if (pin_pcores) {
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
            DWORD_PTR pcore_mask = process_mask & DWORD_PTR{0xFFFF};
            if (pcore_mask != 0) {
                SetProcessAffinityMask(GetCurrentProcess(), pcore_mask);
            }
        }
    }
#endif
    VtunePause(simd_only);
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
    table_info->table_->Reserve(NUM_ROWS);
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
    auto fc_q1 = jit_engine.CompileBatchCountExpression(filter_q1.get());

    auto f_q2 = jit_engine.CompileExpression(filter_q2.get());
    auto fb_q2 = jit_engine.CompileBatchExpression(filter_q2.get());
    auto fc_q2 = jit_engine.CompileBatchCountExpression(filter_q2.get());

    auto f_q3 = jit_engine.CompileExpression(filter_q3.get());
    auto fb_q3 = jit_engine.CompileBatchExpression(filter_q3.get());
    auto fc_q3 = jit_engine.CompileBatchCountExpression(filter_q3.get());

    auto f_q4 = jit_engine.CompileExpression(filter_q4.get());
    auto fb_q4 = jit_engine.CompileBatchExpression(filter_q4.get());
    auto fc_q4 = jit_engine.CompileBatchCountExpression(filter_q4.get());

    std::cout << "JIT Compilation done." << std::endl;
    const unsigned default_workers = std::max(1u, std::min(std::thread::hardware_concurrency(), 8u));
    const unsigned worker_count = requested_workers == 0 ? default_workers : requested_workers;
    BenchmarkThreadPool scan_pool(worker_count, pin_pcores);
    std::vector<ScanWorkerContext> worker_contexts;
    worker_contexts.reserve(worker_count);
    for (unsigned worker = 0; worker < worker_count; ++worker) {
        worker_contexts.emplace_back(schema.GetColumnCount(), STANDARD_VECTOR_SIZE);
    }
    if (simd_only && repeat_count > 1) {
        std::cout << "SIMD repeat count: " << repeat_count << std::endl;
    }
    std::cout << "Parallel workers: " << worker_count << (pin_pcores ? " (P-core affinity)" : "") << std::endl;

    auto test_scenario = [&](const std::string &scenario_name,
                             const AbstractExpression *predicate,
                             JitEngine::CompiledExpressionFunc func,
                             JitEngine::CompiledBatchFunc batch_func,
                             JitEngine::CompiledBatchCountFunc count_func) {
        std::cout << "\n--- Scenario: " << scenario_name << " ---" << std::endl;
        size_t grand_match = 0;
        double time_volcano = 0.0;

        if (!simd_only) {
        time_volcano = time_it([&]() {
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
            size_t match = 0;
            const size_t batch_size = STANDARD_VECTOR_SIZE;
            Chunk chunk(batch_size);
            for (uint32_t i = 0; i < schema.GetColumnCount(); ++i) {
                chunk.AddVector(std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, batch_size));
            }
            std::vector<FlatVector<int32_t> *> flat_vectors(schema.GetColumnCount());
            for (uint32_t i = 0; i < schema.GetColumnCount(); ++i) {
                flat_vectors[i] = static_cast<FlatVector<int32_t> *>(chunk.GetVector(i).get());
            }

            const size_t total_rows = table_info->table_->GetRowCount();
            std::vector<std::vector<int32_t>> buffers(schema.GetColumnCount(), std::vector<int32_t>(batch_size));
            auto eval_result = std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, batch_size);
            std::shared_ptr<Vector> eval_vector = eval_result;

            for (size_t current_idx = 0; current_idx < total_rows; current_idx += batch_size) {
                size_t row_count = std::min(batch_size, total_rows - current_idx);
                chunk.Reset();
                for (size_t col_idx = 0; col_idx < schema.GetColumnCount(); ++col_idx) {
                    const int32_t *src = table_info->table_->UnpackColumnBatch(col_idx, current_idx, row_count, buffers[col_idx].data());
                    std::memcpy(flat_vectors[col_idx]->Data(), src, row_count * sizeof(int32_t));
                }
                chunk.SetCount(row_count);

                predicate->Evaluate(chunk, eval_vector);

                for (size_t i = 0; i < row_count; ++i) {
                    if (eval_result->GetValue(i).GetAsInteger() != 0) {
                        match++;
                    }
                }
            } 
        });
        std::cout << "3. Vectorized (Chunk NO JIT): \t" << time_vectorized << " ms" << std::endl;
        }

        size_t simd_match = 0;
        double time_simd = time_it([&]() {
            if (parallel_only) {
                return;
            }
            VtuneResume(simd_only);
            size_t match = 0;
            const size_t batch_size = STANDARD_VECTOR_SIZE;
            const size_t total_rows = table_info->table_->GetRowCount();
            std::vector<const int32_t *> cols(schema.GetColumnCount());
            std::vector<int32_t> results(batch_size);
            std::vector<uint8_t> nulls(batch_size, 0);
            std::vector<std::vector<int32_t>> buffers(schema.GetColumnCount(), std::vector<int32_t>(batch_size));

            for (size_t repeat = 0; repeat < repeat_count; ++repeat) {
            for (size_t current_idx = 0; current_idx < total_rows; current_idx += batch_size) {
                size_t row_count = std::min(batch_size, total_rows - current_idx);
                for (size_t col_idx = 0; col_idx < schema.GetColumnCount(); ++col_idx) {
                    cols[col_idx] = table_info->table_->UnpackColumnBatch(col_idx, current_idx, row_count, buffers[col_idx].data());
                }

                if (count_func) {
                    match += count_func(reinterpret_cast<const void**>(cols.data()), row_count);
                } else {
                    batch_func(reinterpret_cast<const void**>(cols.data()), results.data(), nulls.data(), row_count);
                    match += CountNonZeroInt32(results.data(), row_count);
                }
            } 
            }
            simd_match = match;
            VtunePause(simd_only);
        });
        if (!parallel_only) {
            std::cout << "4. SIMD (Chunk WITH JIT): \t" << time_simd << " ms total / " << (time_simd / repeat_count) << " ms avg | Matches/run: " << (simd_match / repeat_count) << std::endl;
        }

        size_t parallel_match = 0;
        double time_parallel_simd = time_it([&]() {
            VtuneResume(simd_only);
            const size_t total_rows = table_info->table_->GetRowCount();
            const size_t batch_size = STANDARD_VECTOR_SIZE;
            const size_t rows_per_worker = (total_rows + worker_count - 1) / worker_count;
            parallel_match = scan_pool.Run([&](size_t worker) {
                const size_t begin = worker * rows_per_worker;
                const size_t end = std::min(total_rows, begin + rows_per_worker);
                if (begin >= end) {
                    return size_t{0};
                }

                size_t local_match = 0;
                auto &ctx = worker_contexts[worker];

                for (size_t repeat = 0; repeat < repeat_count; ++repeat) {
                for (size_t current_idx = begin; current_idx < end; current_idx += batch_size) {
                    size_t row_count = std::min(batch_size, end - current_idx);
                    for (size_t col_idx = 0; col_idx < schema.GetColumnCount(); ++col_idx) {
                        ctx.cols[col_idx] = table_info->table_->UnpackColumnBatch(col_idx, current_idx, row_count, ctx.buffers[col_idx].data());
                    }

                    if (count_func) {
                        local_match += count_func(reinterpret_cast<const void**>(ctx.cols.data()), row_count);
                    } else {
                        batch_func(reinterpret_cast<const void**>(ctx.cols.data()), ctx.results.data(), ctx.nulls.data(), row_count);
                        local_match += CountNonZeroInt32(ctx.results.data(), row_count);
                    }
                }
                }
                return local_match;
            });
            VtunePause(simd_only);
        });
        std::cout << "5. Parallel SIMD (JIT): \t" << time_parallel_simd << " ms total / " << (time_parallel_simd / repeat_count) << " ms avg | Matches/run: " << (parallel_match / repeat_count) << std::endl;

        if (!simd_only) {
            std::cout << "-> Speedup (Volcano vs SIMD JIT): " << (time_volcano / time_simd) << "x" << std::endl;
            std::cout << "-> Speedup (Volcano vs Parallel SIMD): " << (time_volcano / time_parallel_simd) << "x" << std::endl;
        }
    };

    test_scenario("Q1: id = 500000", filter_q1.get(), f_q1, fb_q1, fc_q1);
    test_scenario("Q2: age > 40 AND department_id = 5", filter_q2.get(), f_q2, fb_q2, fc_q2);
    test_scenario("Q3: salary + bonus > 100000", filter_q3.get(), f_q3, fb_q3, fc_q3);
    test_scenario("Q4: (salary / 100) + (bonus / 50) > 1000 AND age < 30", filter_q4.get(), f_q4, fb_q4, fc_q4);

    delete catalog;
    delete bpm;
    delete disk_manager;
    remove(db_file.c_str());

    return 0;
}
