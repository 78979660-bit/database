#pragma once

#include "AbstractExecutor.h"
#include "../Plans/HashJoinPlanNode.h"
#include "../ExecutionThreadPool.h"
#include "../../Type/Value.h"
#include "../../Storage/Tuple/Tuple.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <future>
#include <mutex>
#include <cmath>
#include <immintrin.h>

namespace Database
{

    struct SimdBloomFilter
    {
        struct alignas(32) Block
        {
            uint64_t data[4];
        };
        std::vector<Block> blocks;
        size_t num_blocks;

        SimdBloomFilter(size_t expected_elements)
        {
            size_t min_bits = expected_elements * 8;
            num_blocks = (min_bits + 255) / 256;
            if (num_blocks == 0)
                num_blocks = 1;
            size_t power_of_2 = 1;
            while (power_of_2 < num_blocks)
                power_of_2 <<= 1;
            num_blocks = power_of_2;
            blocks.resize(num_blocks);
            for (size_t i = 0; i < num_blocks; ++i)
            {
                blocks[i].data[0] = 0;
                blocks[i].data[1] = 0;
                blocks[i].data[2] = 0;
                blocks[i].data[3] = 0;
            }
        }

        void Insert(size_t hash_val)
        {
            size_t block_idx = hash_val & (num_blocks - 1);
            uint32_t *ptr = (uint32_t *)&blocks[block_idx];
            size_t h = hash_val >> 16 ^ hash_val;
            ptr[(h >> 0) & 7] |= (1u << ((h >> 3) & 31));
            ptr[(h >> 8) & 7] |= (1u << ((h >> 11) & 31));
            ptr[(h >> 16) & 7] |= (1u << ((h >> 19) & 31));
            ptr[(h >> 24) & 7] |= (1u << ((h >> 27) & 31));
        }

        bool Probe(size_t hash_val) const
        {
            size_t block_idx = hash_val & (num_blocks - 1);
            const uint32_t *ptr = (const uint32_t *)&blocks[block_idx];
            size_t h = hash_val >> 16 ^ hash_val;
            if (!(ptr[(h >> 0) & 7] & (1u << ((h >> 3) & 31))))
                return false;
            if (!(ptr[(h >> 8) & 7] & (1u << ((h >> 11) & 31))))
                return false;
            if (!(ptr[(h >> 16) & 7] & (1u << ((h >> 19) & 31))))
                return false;
            if (!(ptr[(h >> 24) & 7] & (1u << ((h >> 27) & 31))))
                return false;
            return true;
        }
    };

    struct PRHJValueHash
    {
        size_t operator()(const Value &val) const
        {
            if (val.GetTypeId() == TypeId::INTEGER)
            {
                return std::hash<int32_t>()(val.GetAsInteger());
            }
            else if (val.GetTypeId() == TypeId::VARCHAR)
            {
                return std::hash<std::string>()(val.GetAsVarchar());
            }
            return 0;
        }
    };

    class ParallelRadixHashJoinExecutor : public AbstractExecutor
    {
    public:
        static constexpr size_t NUM_RADIX_BITS = 4;
        static constexpr size_t NUM_PARTITIONS = 1 << NUM_RADIX_BITS;

        ParallelRadixHashJoinExecutor(ExecutorContext *exec_ctx, const HashJoinPlanNode *plan,
                                      std::unique_ptr<AbstractExecutor> &&child_left,
                                      std::unique_ptr<AbstractExecutor> &&child_right)
            : AbstractExecutor(exec_ctx), plan_(plan),
              left_executor_(std::move(child_left)), right_executor_(std::move(child_right)) {}

        void Init() override
        {
            left_executor_->Init();
            right_executor_->Init();

            left_col_idx_ = plan_->GetLeftPlan()->GetOutputSchema()->GetColumnIndex(plan_->GetLeftJoinColName());
            right_col_idx_ = plan_->GetRightPlan()->GetOutputSchema()->GetColumnIndex(plan_->GetRightJoinColName());

            // 1. Materialize entire left & right for parallel partitioning
            std::vector<Tuple> all_left;
            std::vector<Tuple> all_right;

            Tuple tuple;
            RID rid;
            while (left_executor_->Next(&tuple, &rid))
            {
                all_left.push_back(tuple);
            }

            SimdBloomFilter bloom(std::max<size_t>(1024, all_left.size()));
            PRHJValueHash hasher;
            const Schema *left_schema = plan_->GetLeftPlan()->GetOutputSchema();
            for (const auto &t : all_left)
            {
                Value val = t.GetValue(left_schema, left_col_idx_);
                bloom.Insert(hasher(val));
            }

            const Schema *right_schema = plan_->GetRightPlan()->GetOutputSchema();
            while (right_executor_->Next(&tuple, &rid))
            {
                Value val = tuple.GetValue(right_schema, right_col_idx_);
                if (bloom.Probe(hasher(val)))
                {
                    all_right.push_back(tuple);
                }
            }

            // 2. Parallel Radix Partitioning
            std::vector<std::vector<Tuple>> left_partitions(NUM_PARTITIONS);
            std::vector<std::vector<Tuple>> right_partitions(NUM_PARTITIONS);
            std::vector<std::unique_ptr<std::mutex>> left_mutexes;
            std::vector<std::unique_ptr<std::mutex>> right_mutexes;
            for (size_t i = 0; i < NUM_PARTITIONS; ++i)
            {
                left_mutexes.push_back(std::make_unique<std::mutex>());
                right_mutexes.push_back(std::make_unique<std::mutex>());
            }

            auto partition_task = [&](const std::vector<Tuple> &source, size_t col_idx,
                                      std::vector<std::vector<Tuple>> &partitions,
                                      std::vector<std::unique_ptr<std::mutex>> &mutexes, const Schema *schema)
            {
                std::vector<std::vector<Tuple>> local_parts(NUM_PARTITIONS);
                PRHJValueHash hasher;

                for (const auto &t : source)
                {
                    Value val = t.GetValue(schema, col_idx);
                    size_t hash_val = hasher(val);
                    size_t partition_idx = hash_val & (NUM_PARTITIONS - 1);
                    local_parts[partition_idx].push_back(t);
                }

                for (size_t i = 0; i < NUM_PARTITIONS; ++i)
                {
                    if (!local_parts[i].empty())
                    {
                        std::lock_guard<std::mutex> lock(*mutexes[i]);
                        partitions[i].insert(partitions[i].end(),
                                             local_parts[i].begin(), local_parts[i].end());
                    }
                }
            };

            auto &pool = ExecutionThreadPool::GetInstance();
            size_t num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0)
                num_threads = 4;

            auto run_parallel_partition = [&](const std::vector<Tuple> &source, size_t col_idx,
                                              std::vector<std::vector<Tuple>> &partitions,
                                              std::vector<std::unique_ptr<std::mutex>> &mutexes, const Schema *schema)
            {
                if (source.empty())
                    return;
                size_t chunk_size = source.size() / num_threads + 1;
                std::vector<std::future<void>> futures;

                for (size_t i = 0; i < num_threads; ++i)
                {
                    size_t start = i * chunk_size;
                    size_t end = std::min(start + chunk_size, source.size());
                    if (start >= source.size())
                        break;

                    futures.push_back(pool.Enqueue([&, start, end, col_idx, schema](int numa)
                                                   {
                        std::vector<Tuple> local_slice(source.begin() + start, source.begin() + end);
                        partition_task(local_slice, col_idx, partitions, mutexes, schema); }));
                }

                for (auto &f : futures)
                {
                    f.wait();
                }
            };

            run_parallel_partition(all_left, left_col_idx_, left_partitions, left_mutexes, plan_->GetLeftPlan()->GetOutputSchema());
            run_parallel_partition(all_right, right_col_idx_, right_partitions, right_mutexes, plan_->GetRightPlan()->GetOutputSchema());

            // 3. Parallel Build & Probe phase over discrete partitions
            std::mutex output_mutex;
            std::vector<std::future<void>> join_futures;

            for (size_t i = 0; i < NUM_PARTITIONS; ++i)
            {
                join_futures.push_back(pool.Enqueue([&, i](int numa)
                                                    {
                    if (left_partitions[i].empty() || right_partitions[i].empty()) return;

                    const Schema* l_schema = plan_->GetLeftPlan()->GetOutputSchema();
                    const Schema* r_schema = plan_->GetRightPlan()->GetOutputSchema();

                    // [数据结构黑魔法: 扁平基数哈希探测]
                    // 彻底抛弃自带大分配负担且对 L1/L2 Cache 极其不友好的 std::unordered_map
                    // 因为我们已经先做了 Parallel Radix Partition，每个 Partition 极小
                    // 我们直接用一个刚好能被塞进 L1 Cache (约32~64KB) 的二维扁平连续数组，
                    // 以开放位掩码计算槽位，进行高速探针！
                    size_t cache_friendly_table_size = 1 << 10; // 固定分配，完美规避树状 rehash 和 pointer chasing
                    std::vector<std::vector<Tuple>> flat_hash_table(cache_friendly_table_size);
                    PRHJValueHash hasher;

                    for (const auto& l_tuple : left_partitions[i]) {
                        Value key = l_tuple.GetValue(l_schema, left_col_idx_);
                        size_t slot_idx = hasher(key) & (cache_friendly_table_size - 1);
                        flat_hash_table[slot_idx].push_back(l_tuple);
                    }

                    std::vector<Tuple> local_results;
                    for (const auto& r_tuple : right_partitions[i]) {
                        Value key = r_tuple.GetValue(r_schema, right_col_idx_);
                        size_t slot_idx = hasher(key) & (cache_friendly_table_size - 1);
                        
                        // 线性 Cache-friendly 探针，无需二叉或链表下探
                        for (const auto& l_match : flat_hash_table[slot_idx]) {
                            if (l_match.GetValue(l_schema, left_col_idx_) == key) {
                                local_results.push_back(AssembleJoinedTuple(&l_match, &r_tuple));
                            }
                        }
                    }

                    if (!local_results.empty()) {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        output_results_.insert(output_results_.end(), local_results.begin(), local_results.end());
                    } }));
            }

            for (auto &f : join_futures)
            {
                f.wait();
            }

            output_index_ = 0;
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            if (output_index_ < output_results_.size())
            {
                *tuple = output_results_[output_index_++];
                *rid = RID();
                return true;
            }
            return false;
        }

    private:
        Tuple AssembleJoinedTuple(const Tuple *left_tuple, const Tuple *right_tuple)
        {
            std::vector<Value> values;
            const Schema *left_schema = plan_->GetLeftPlan()->GetOutputSchema();
            for (uint32_t i = 0; i < left_schema->GetColumnCount(); ++i)
            {
                values.push_back(left_tuple->GetValue(left_schema, i));
            }
            const Schema *right_schema = plan_->GetRightPlan()->GetOutputSchema();
            for (uint32_t i = 0; i < right_schema->GetColumnCount(); ++i)
            {
                values.push_back(right_tuple->GetValue(right_schema, i));
            }
            return Tuple(values, plan_->GetOutputSchema());
        }

        const HashJoinPlanNode *plan_;
        std::unique_ptr<AbstractExecutor> left_executor_;
        std::unique_ptr<AbstractExecutor> right_executor_;

        uint32_t left_col_idx_{0};
        uint32_t right_col_idx_{0};

        std::vector<Tuple> output_results_;
        size_t output_index_{0};
    };

} // namespace Database