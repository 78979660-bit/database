#pragma once

#include "AbstractExecutor.h"
#include "../Plans/SortPlanNode.h"
#include "../../Storage/Tuple/Tuple.h"
#include <vector>
#include <algorithm>
#include <fstream>
#include <queue>
#include <string>

namespace Database
{

    class ExternalMergeSortExecutor : public AbstractExecutor
    {
    public:
        static constexpr size_t MEMORY_LIMIT_TUPLES = 50000;

        ExternalMergeSortExecutor(ExecutorContext *exec_ctx, const SortPlanNode *plan,
                                  std::unique_ptr<AbstractExecutor> &&child_executor)
            : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

        void Init() override
        {
            child_executor_->Init();
            tuples_.clear();
            runs_files_.clear();
            current_run_ = 0;
            tuple_idx_ = 0;

            Tuple tuple;
            bool over_memory = false;

            while (child_executor_->Next(&tuple))
            {
                tuples_.push_back(tuple);

                if (tuples_.size() >= MEMORY_LIMIT_TUPLES)
                {
                    over_memory = true;
                    SortCurrentBatch();
                    SpillRunToDisk();
                    tuples_.clear();
                }
            }

            if (over_memory)
            {
                if (!tuples_.empty())
                {
                    SortCurrentBatch();
                    SpillRunToDisk();
                }
                InitMergePhase();
            }
            else
            {
                SortCurrentBatch();
            }
        }

        bool Next(Tuple *tuple) override
        {
            if (!runs_files_.empty())
            {
                return GetNextFromMerge(tuple);
            }
            else
            {
                if (tuple_idx_ < tuples_.size())
                {
                    *tuple = tuples_[tuple_idx_++];
                    return true;
                }
                return false;
            }
        }

    private:
        struct RunIterator
        {
            std::ifstream file_;
            Tuple current_tuple_;
            bool valid_{false};

            RunIterator(const std::string &filename)
            {
                file_.open(filename, std::ios::binary);
                Advance();
            }

            void Advance()
            {
                if (!file_.is_open() || file_.eof())
                {
                    valid_ = false;
                    return;
                }
                uint32_t len = 0;
                if (!file_.read(reinterpret_cast<char *>(&len), sizeof(len)))
                {
                    valid_ = false;
                    return;
                }
                std::vector<char> data(len);
                file_.read(data.data(), len);

                RID rid;
                file_.read(reinterpret_cast<char *>(&rid), sizeof(rid));

                current_tuple_ = Tuple(data.data(), len, rid);
                valid_ = true;
            }

            ~RunIterator()
            {
                if (file_.is_open())
                    file_.close();
            }
        };

        struct RunIteratorComparator
        {
            const SortPlanNode *plan_;

            RunIteratorComparator(const SortPlanNode *plan) : plan_(plan) {}

            bool operator()(const RunIterator *a, const RunIterator *b) const
            {
                const auto &order_bys = plan_->GetOrderBys();
                const auto &is_asc = plan_->GetIsAscs();
                const Schema *schema = plan_->GetOutputSchema();

                for (size_t i = 0; i < order_bys.size(); i++)
                {
                    uint32_t col_idx = schema->GetColumnIndex(order_bys[i]);
                    Value val_a = a->current_tuple_.GetValue(schema, col_idx);
                    Value val_b = b->current_tuple_.GetValue(schema, col_idx);

                    if (val_a.CompareLess(val_b))
                        return !is_asc[i]; // Reverse for min-heap
                    if (val_b.CompareLess(val_a))
                        return is_asc[i];
                }
                return false;
            }
        };

        void SortCurrentBatch()
        {
            const auto &order_bys = plan_->GetOrderBys();
            const auto &is_asc = plan_->GetIsAscs();
            const Schema *schema = plan_->GetOutputSchema();

            std::sort(tuples_.begin(), tuples_.end(), [&](const Tuple &a, const Tuple &b)
                      {
                for (size_t i = 0; i < order_bys.size(); i++) {
                    uint32_t col_idx = schema->GetColumnIndex(order_bys[i]);
                    Value val_a = a.GetValue(schema, col_idx);
                    Value val_b = b.GetValue(schema, col_idx);

                    if (val_a.CompareLess(val_b)) return is_asc[i];
                    if (val_b.CompareLess(val_a)) return !is_asc[i];
                }
                return false; });
        }

        void SpillRunToDisk()
        {
            std::string file_name = "sort_run_" + std::to_string(current_run_++) + ".tmp";
            std::ofstream out(file_name, std::ios::binary);
            for (const auto &tuple : tuples_)
            {
                uint32_t len = tuple.GetLength();
                out.write(reinterpret_cast<const char *>(&len), sizeof(len));
                out.write(tuple.GetData(), len);
                RID rid = tuple.GetRID();
                out.write(reinterpret_cast<const char *>(&rid), sizeof(rid));
            }
            out.close();
            runs_files_.push_back(file_name);
        }

        void InitMergePhase()
        {
            for (const auto &filename : runs_files_)
            {
                RunIterator *it = new RunIterator(filename);
                if (it->valid_)
                {
                    merge_heap_.push_back(it);
                }
                else
                {
                    delete it;
                }
            }
            std::make_heap(merge_heap_.begin(), merge_heap_.end(), RunIteratorComparator(plan_));
        }

        bool GetNextFromMerge(Tuple *tuple)
        {
            if (merge_heap_.empty())
            {
                return false;
            }

            std::pop_heap(merge_heap_.begin(), merge_heap_.end(), RunIteratorComparator(plan_));
            RunIterator *it = merge_heap_.back();

            *tuple = it->current_tuple_;

            it->Advance();
            if (it->valid_)
            {
                std::push_heap(merge_heap_.begin(), merge_heap_.end(), RunIteratorComparator(plan_));
            }
            else
            {
                merge_heap_.pop_back();
                delete it;
            }

            return true;
        }

        const SortPlanNode *plan_;
        std::unique_ptr<AbstractExecutor> child_executor_;
        std::vector<Tuple> tuples_;
        size_t tuple_idx_{0};

        int current_run_{0};
        std::vector<std::string> runs_files_;
        std::vector<RunIterator *> merge_heap_;
    };

} // namespace Database