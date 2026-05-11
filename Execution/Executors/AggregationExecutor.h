#pragma once

#include "AbstractExecutor.h"
#include "../Plans/AggregationPlanNode.h"
#include "../ExecutionThreadPool.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <future>
#include <mutex>
#include <algorithm>

namespace Database
{

    struct AggregateKey
    {
        std::vector<Value> group_bys_;

        bool operator==(const AggregateKey &other) const
        {
            if (group_bys_.size() != other.group_bys_.size())
            {
                return false;
            }
            for (size_t i = 0; i < group_bys_.size(); ++i)
            {
                if (!(group_bys_[i] == other.group_bys_[i]))
                {
                    return false;
                }
            }
            return true;
        }
    };

    struct AggregateKeyHash
    {
        size_t operator()(const AggregateKey &key) const
        {
            size_t curr_hash = 0;
            for (const auto &val : key.group_bys_)
            {
                size_t hash_val = 0;
                if (val.GetTypeId() == TypeId::INTEGER)
                {
                    hash_val = std::hash<int32_t>()(val.GetAsInteger());
                }
                else if (val.GetTypeId() == TypeId::VARCHAR)
                {
                    hash_val = std::hash<std::string>()(val.GetAsVarchar());
                }
                // simple hash combine
                curr_hash ^= hash_val + 0x9e3779b9 + (curr_hash << 6) + (curr_hash >> 2);
            }
            return curr_hash;
        }
    };

    struct AggregateValue
    {
        std::vector<Value> aggregates_;
    };

    class SimpleAggregationHashTable
    {
    public:
        SimpleAggregationHashTable(const std::vector<std::shared_ptr<AbstractExpression>> &agg_exprs,
                                   const std::vector<AggregationType> &agg_types)
            : agg_exprs_(agg_exprs), agg_types_(agg_types) {}

        AggregateValue GenerateInitialAggregateValue()
        {
            std::vector<Value> values;
            for (auto agg_type : agg_types_)
            {
                switch (agg_type)
                {
                case AggregationType::CountStar:
                    values.emplace_back(Value(0));
                    break;
                case AggregationType::Count:
                case AggregationType::Sum:
                case AggregationType::Min:
                case AggregationType::Max:
                    values.emplace_back(Value(TypeId::INTEGER)); // Null or initial state
                    break;
                }
            }
            return {values};
        }

        void CombineAggregateValues(AggregateValue *result, const AggregateValue &input, bool is_merge = false)
        {
            for (size_t i = 0; i < agg_exprs_.size(); ++i)
            {
                switch (agg_types_[i])
                {
                case AggregationType::CountStar:
                    result->aggregates_[i] = Value(result->aggregates_[i].GetAsInteger() + (is_merge ? input.aggregates_[i].GetAsInteger() : 1));
                    break;
                case AggregationType::Count:
                    if (input.aggregates_[i].GetTypeId() != TypeId::INVALID)
                    {
                        if (result->aggregates_[i].GetTypeId() == TypeId::INVALID)
                        {
                            result->aggregates_[i] = is_merge ? input.aggregates_[i] : Value(1);
                        }
                        else
                        {
                            result->aggregates_[i] = Value(result->aggregates_[i].GetAsInteger() + (is_merge ? input.aggregates_[i].GetAsInteger() : 1));
                        }
                    }
                    break;
                case AggregationType::Sum:
                    if (input.aggregates_[i].GetTypeId() != TypeId::INVALID)
                    {
                        if (result->aggregates_[i].GetTypeId() == TypeId::INVALID)
                        {
                            result->aggregates_[i] = input.aggregates_[i];
                        }
                        else
                        {
                            result->aggregates_[i] = Value(result->aggregates_[i].GetAsInteger() + input.aggregates_[i].GetAsInteger());
                        }
                    }
                    break;
                case AggregationType::Min:
                    if (input.aggregates_[i].GetTypeId() != TypeId::INVALID)
                    {
                        if (result->aggregates_[i].GetTypeId() == TypeId::INVALID || input.aggregates_[i] < result->aggregates_[i])
                        {
                            result->aggregates_[i] = input.aggregates_[i];
                        }
                    }
                    break;
                case AggregationType::Max:
                    if (input.aggregates_[i].GetTypeId() != TypeId::INVALID)
                    {
                        if (result->aggregates_[i].GetTypeId() == TypeId::INVALID || result->aggregates_[i] < input.aggregates_[i])
                        {
                            result->aggregates_[i] = input.aggregates_[i];
                        }
                    }
                    break;
                }
            }
        }

        void InsertCombine(const AggregateKey &agg_key, const AggregateValue &agg_val)
        {
            if (ht_.count(agg_key) == 0)
            {
                ht_.insert({agg_key, GenerateInitialAggregateValue()});
            }
            CombineAggregateValues(&ht_[agg_key], agg_val);
        }

        void MergeLocalTable(const SimpleAggregationHashTable &other)
        {
            for (auto it = other.Begin(); it != other.End(); ++it)
            {
                if (ht_.count(it->first) == 0)
                {
                    ht_.insert({it->first, it->second});
                }
                else
                {
                    CombineAggregateValues(&ht_[it->first], it->second, true);
                }
            }
        }

        std::unordered_map<AggregateKey, AggregateValue, AggregateKeyHash>::const_iterator Begin() const
        {
            return ht_.begin();
        }

        std::unordered_map<AggregateKey, AggregateValue, AggregateKeyHash>::const_iterator End() const
        {
            return ht_.end();
        }

        void Clear() { ht_.clear(); }

    private:
        std::unordered_map<AggregateKey, AggregateValue, AggregateKeyHash> ht_{};
        const std::vector<std::shared_ptr<AbstractExpression>> &agg_exprs_;
        const std::vector<AggregationType> &agg_types_;
    };

    class AggregationExecutor : public AbstractExecutor
    {
    public:
        AggregationExecutor(ExecutorContext *exec_ctx,
                            const AggregationPlanNode *plan,
                            std::unique_ptr<AbstractExecutor> &&child)
            : AbstractExecutor(exec_ctx),
              plan_(plan),
              child_(std::move(child)),
              aht_(plan_->GetAggregates(), plan_->GetAggregateTypes()),
              aht_iterator_(aht_.Begin())
        {
        }

        void Init() override
        {
            child_->Init();
            aht_.Clear();

            Tuple tuple;
            RID rid;

            // 1. Materialize all tuples locally first.
            std::vector<Tuple> all_tuples;
            while (child_->Next(&tuple, &rid))
            {
                all_tuples.push_back(tuple);
            }

            if (all_tuples.empty() && plan_->GetGroupBys().empty())
            {
                aht_.InsertCombine(AggregateKey{std::vector<Value>{}}, aht_.GenerateInitialAggregateValue());
                aht_iterator_ = aht_.Begin();
                return;
            }

            auto &pool = ExecutionThreadPool::GetInstance();
            size_t num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0)
                num_threads = 4;

            // 2. Thread-local Hash Aggregation mapping
            size_t chunk_size = all_tuples.size() / num_threads + 1;
            std::vector<std::future<std::unique_ptr<SimpleAggregationHashTable>>> futures;

            for (size_t i = 0; i < num_threads; ++i)
            {
                size_t start = i * chunk_size;
                size_t end = std::min(start + chunk_size, all_tuples.size());
                if (start >= all_tuples.size())
                    break;

                futures.push_back(pool.Enqueue([&, start, end](int numa)
                                               {
                    auto local_aht = std::make_unique<SimpleAggregationHashTable>(plan_->GetAggregates(), plan_->GetAggregateTypes());
                    for (size_t j = start; j < end; ++j)
                    {
                        AggregateKey agg_key = MakeAggregateKey(&all_tuples[j]);
                        AggregateValue agg_val = MakeAggregateValue(&all_tuples[j]);
                        local_aht->InsertCombine(agg_key, agg_val);
                    }
                    return local_aht; }));
            }

            // 3. Global reduction (Merge thread-local structures)
            for (auto &f : futures)
            {
                auto local_aht = f.get();
                aht_.MergeLocalTable(*local_aht);
            }

            aht_iterator_ = aht_.Begin();
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            if (aht_iterator_ == aht_.End())
            {
                return false;
            }

            std::vector<Value> values;
            // output schema typically matches group_bys followed by aggregates
            for (const auto &val : aht_iterator_->first.group_bys_)
            {
                values.push_back(val);
            }
            for (const auto &val : aht_iterator_->second.aggregates_)
            {
                // if it's invalid (e.g. sum of no elements), emit something sensible, like 0
                if (val.GetTypeId() == TypeId::INVALID)
                {
                    values.push_back(Value(0));
                }
                else
                {
                    values.push_back(val);
                }
            }

            *tuple = Tuple(values, plan_->GetOutputSchema());
            *rid = RID(); // Dummy RID

            ++aht_iterator_;
            return true;
        }

        // Vectorized Next
        bool Next(Chunk &chunk) override
        {
            size_t output_count = 0;
            const size_t batch_size = chunk.GetCapacity();

            while (aht_iterator_ != aht_.End() && output_count < batch_size)
            {
                size_t out_col_idx = 0;

                // Group by columns
                for (const auto &val : aht_iterator_->first.group_bys_)
                {
                    chunk.GetVector(out_col_idx++)->SetValue(output_count, val);
                }

                // Aggregate columns
                for (const auto &val : aht_iterator_->second.aggregates_)
                {
                    if (val.GetTypeId() == TypeId::INVALID)
                    {
                        chunk.GetVector(out_col_idx++)->SetValue(output_count, Value(0));
                    }
                    else
                    {
                        chunk.GetVector(out_col_idx++)->SetValue(output_count, val);
                    }
                }

                output_count++;
                ++aht_iterator_;
            }

            chunk.SetCount(output_count);
            return output_count > 0;
        }

    private:
        AggregateKey MakeAggregateKey(const Tuple *tuple)
        {
            std::vector<Value> keys;
            for (const auto &expr : plan_->GetGroupBys())
            {
                keys.push_back(expr->Evaluate(tuple, plan_->GetChildPlan()->GetOutputSchema()));
            }
            return {keys};
        }

        AggregateValue MakeAggregateValue(const Tuple *tuple)
        {
            std::vector<Value> vals;
            for (const auto &expr : plan_->GetAggregates())
            {
                if (expr)
                {
                    vals.push_back(expr->Evaluate(tuple, plan_->GetChildPlan()->GetOutputSchema()));
                }
                else
                { // Handle CountStar which might not have an expression
                    vals.push_back(Value(0));
                }
            }
            return {vals};
        }

        const AggregationPlanNode *plan_;
        std::unique_ptr<AbstractExecutor> child_;
        SimpleAggregationHashTable aht_;
        std::unordered_map<AggregateKey, AggregateValue, AggregateKeyHash>::const_iterator aht_iterator_;
    };

} // namespace Database