#pragma once

#include "AbstractExecutor.h"
#include "../Plans/HashJoinPlanNode.h"
#include "../../DiskManager.h"
#include "../../BufferPoolManager.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <string>

namespace Database
{

    class GraceHashJoinExecutor : public AbstractExecutor
    {
    public:
        // Adjust these variables properly in production environments
        static constexpr size_t NUM_PARTITIONS = 16;
        static constexpr size_t IN_MEMORY_TUPLE_LIMIT = 10000;

        GraceHashJoinExecutor(ExecutorContext *exec_ctx, const HashJoinPlanNode *plan,
                              std::unique_ptr<AbstractExecutor> &&left_child,
                              std::unique_ptr<AbstractExecutor> &&right_child)
            : AbstractExecutor(exec_ctx), plan_(plan),
              left_executor_(std::move(left_child)), right_executor_(std::move(right_child)) {}

        void Init() override
        {
            left_executor_->Init();
            right_executor_->Init();

            left_col_idx_ = plan_->GetLeftPlan()->GetOutputSchema()->GetColumnIndex(plan_->GetLeftJoinColName());
            right_col_idx_ = plan_->GetRightPlan()->GetOutputSchema()->GetColumnIndex(plan_->GetRightJoinColName());

            // Phase 1: Partitioning
            PartitionChild(left_executor_.get(), "left", left_col_idx_);
            PartitionChild(right_executor_.get(), "right", right_col_idx_);

            // Phase 2: Probe setup
            current_partition_idx_ = 0;
            LoadNextPartition();
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            while (true)
            {
                if (current_matches_ && match_idx_ < current_matches_->size())
                {
                    *tuple = AssembleJoinedTuple(&((*current_matches_)[match_idx_]), &right_tuple_);
                    *rid = RID();
                    match_idx_++;
                    return true;
                }

                if (!AdvanceRightTuple())
                {
                    // Current partition exhausted. Load the next one.
                    current_partition_idx_++;
                    if (current_partition_idx_ >= NUM_PARTITIONS)
                    {
                        return false;
                    }
                    LoadNextPartition();
                    continue; // Re-evaluate loop with new partition
                }
            }
        }

        // Vectorized Next
        bool Next(Chunk &chunk) override
        {
            size_t output_count = 0;
            const size_t batch_size = chunk.GetCapacity();

            while (output_count < batch_size)
            {
                if (current_matches_ && match_idx_ < current_matches_->size())
                {
                    const Tuple &left_tuple = (*current_matches_)[match_idx_];
                    const Schema *left_schema = plan_->GetLeftPlan()->GetOutputSchema();
                    const Schema *right_schema = plan_->GetRightPlan()->GetOutputSchema();
                    size_t out_col_idx = 0;

                    for (uint32_t i = 0; i < left_schema->GetColumnCount(); ++i)
                    {
                        chunk.GetVector(out_col_idx++)->SetValue(output_count, left_tuple.GetValue(left_schema, i));
                    }
                    for (uint32_t i = 0; i < right_schema->GetColumnCount(); ++i)
                    {
                        chunk.GetVector(out_col_idx++)->SetValue(output_count, right_tuple_.GetValue(right_schema, i));
                    }

                    output_count++;
                    match_idx_++;
                }
                else
                {
                    if (!AdvanceRightTuple())
                    {
                        current_partition_idx_++;
                        if (current_partition_idx_ >= NUM_PARTITIONS)
                        {
                            break;
                        }
                        LoadNextPartition();
                    }
                }
            }

            chunk.SetCount(output_count);
            return output_count > 0;
        }

    private:
        void PartitionChild(AbstractExecutor *child, const std::string &side, int col_idx)
        {
            std::vector<std::ofstream> files;
            for (size_t i = 0; i < NUM_PARTITIONS; ++i)
            {
                // In robust systems, DiskManager should handle temp files to avoid IO resource leaks.
                // We use fstream for quick prototype demonstration of external memory layout.
                std::string filename = "temp_ghj_" + side + "_" + std::to_string(i) + ".dat";
                files.emplace_back(filename, std::ios::binary | std::ios::out | std::ios::trunc);
            }

            Chunk chunk(STANDARD_VECTOR_SIZE);
            const Schema *schema = side == "left" ? plan_->GetLeftPlan()->GetOutputSchema() : plan_->GetRightPlan()->GetOutputSchema();

            // Simulate fallback row extraction
            while (child->Next(chunk))
            {
                for (size_t i = 0; i < chunk.GetCount(); ++i)
                {
                    size_t actual_idx = chunk.HasSelectionVector() ? chunk.GetSelectionVector()->GetIndex(i) : i;

                    std::vector<Value> values;
                    for (size_t c = 0; c < chunk.GetColumnCount(); ++c)
                    {
                        values.push_back(chunk.GetVector(c)->GetValue(actual_idx));
                    }

                    Tuple t(values, schema);

                    // Hash partition
                    Value join_val = values[col_idx];
                    size_t partition_id = ValueHash()(join_val) % NUM_PARTITIONS;

                    uint32_t len = t.GetLength();
                    files[partition_id].write(reinterpret_cast<const char *>(&len), sizeof(uint32_t));
                    files[partition_id].write(t.GetData(), len);
                }
                chunk.Reset();
            }

            for (auto &f : files)
            {
                f.close();
            }
        }

        void LoadNextPartition()
        {
            hash_table_.clear();
            if (current_right_file_.is_open())
            {
                current_right_file_.close();
            }

            if (current_partition_idx_ >= NUM_PARTITIONS)
                return;

            std::string left_file = "temp_ghj_left_" + std::to_string(current_partition_idx_) + ".dat";
            std::ifstream left_in(left_file, std::ios::binary | std::ios::in);

            if (left_in.is_open())
            {
                uint32_t len;
                while (left_in.read(reinterpret_cast<char *>(&len), sizeof(uint32_t)))
                {
                    std::vector<char> buffer(len);
                    left_in.read(buffer.data(), len);
                    Tuple t(buffer.data(), len, RID());

                    Value join_val = t.GetValue(plan_->GetLeftPlan()->GetOutputSchema(), left_col_idx_);
                    hash_table_[join_val].push_back(t);
                }
                left_in.close();
                std::remove(left_file.c_str()); // Clean up temp file
            }

            std::string right_file = "temp_ghj_right_" + std::to_string(current_partition_idx_) + ".dat";
            current_right_file_.open(right_file, std::ios::binary | std::ios::in);

            match_idx_ = 0;
            current_matches_ = nullptr;
        }

        bool AdvanceRightTuple()
        {
            if (!current_right_file_.is_open() || current_right_file_.eof())
            {
                return false;
            }

            uint32_t len;
            if (current_right_file_.read(reinterpret_cast<char *>(&len), sizeof(uint32_t)))
            {
                std::vector<char> buffer(len);
                current_right_file_.read(buffer.data(), len);
                right_tuple_ = Tuple(buffer.data(), len, RID());

                Value right_val = right_tuple_.GetValue(plan_->GetRightPlan()->GetOutputSchema(), right_col_idx_);
                auto it = hash_table_.find(right_val);
                if (it != hash_table_.end())
                {
                    current_matches_ = &(it->second);
                    match_idx_ = 0;
                }
                else
                {
                    current_matches_ = nullptr;
                }
                return true;
            }

            return false;
        }

        Tuple AssembleJoinedTuple(const Tuple *left, const Tuple *right)
        {
            std::vector<Value> values;
            const Schema *left_schema = plan_->GetLeftPlan()->GetOutputSchema();
            for (uint32_t i = 0; i < left_schema->GetColumnCount(); ++i)
            {
                values.push_back(left->GetValue(left_schema, i));
            }
            const Schema *right_schema = plan_->GetRightPlan()->GetOutputSchema();
            for (uint32_t i = 0; i < right_schema->GetColumnCount(); ++i)
            {
                values.push_back(right->GetValue(right_schema, i));
            }
            return Tuple(values, plan_->GetOutputSchema());
        }

        const HashJoinPlanNode *plan_;
        std::unique_ptr<AbstractExecutor> left_executor_;
        std::unique_ptr<AbstractExecutor> right_executor_;

        int left_col_idx_{-1};
        int right_col_idx_{-1};

        // Phase 1 partitioning configurations
        size_t current_partition_idx_{0};

        // Phase 2 probing objects
        std::unordered_map<Value, std::vector<Tuple>, ValueHash> hash_table_;
        std::ifstream current_right_file_;
        Tuple right_tuple_;
        std::vector<Tuple> *current_matches_{nullptr};
        size_t match_idx_{0};
    };

} // namespace Database