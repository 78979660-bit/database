#pragma once

#include "AbstractExecutor.h"
#include "../Plans/FilterPlanNode.h"
#include "../JIT/JitEngine.h"

namespace Database
{

    class FilterExecutor : public AbstractExecutor
    {
    public:
        FilterExecutor(ExecutorContext *exec_ctx, const FilterPlanNode *plan,
                       std::unique_ptr<AbstractExecutor> &&child_executor)
            : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}
        void Init() override
        {
            if (child_executor_)
                child_executor_->Init();

            const AbstractExpression *filter_expr = plan_->GetPredicate();
            if (filter_expr && !compiled_func_ && !compiled_batch_func_)
            {
                compiled_func_ = jit_engine_.CompileExpression(filter_expr);
                compiled_batch_func_ = jit_engine_.CompileBatchExpression(filter_expr);
            }
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            Tuple child_tuple;
            RID child_rid;

            while (child_executor_->Next(&child_tuple, &child_rid))
            {
                if (compiled_func_)
                {
                    const int32_t *row_data = reinterpret_cast<const int32_t *>(child_tuple.GetData());
                    int32_t res;
                    uint8_t is_null;
                    compiled_func_(row_data, &res, &is_null);
                    if (is_null)
                        res = 0;
                    if (res == 0)
                        continue;
                }
                else
                {
                    const AbstractExpression *filter_expr = plan_->GetPredicate();
                    const Schema *schema = plan_->GetChildPlan()->GetOutputSchema();

                    if (filter_expr != nullptr)
                    {
                        Value val = filter_expr->Evaluate(&child_tuple, schema);
                        if (val.GetAsInteger() == 0) // Treat 0 as false
                        {
                            continue;
                        }
                    }
                }

                *tuple = child_tuple;
                *rid = child_rid;
                return true;
            }

            return false;
        }
        bool Next(Chunk &chunk) override
        {
            const AbstractExpression *filter_expr = plan_->GetPredicate();

            while (child_executor_->Next(chunk))
            {
                if (filter_expr == nullptr)
                {
                    return true;
                }

                size_t matched_count = 0;
                auto sel_vector = chunk.GetSelectionVector();
                if (!sel_vector)
                {
                    sel_vector = std::make_shared<SelectionVector>(chunk.GetCapacity());
                    for (size_t i = 0; i < chunk.GetCount(); ++i)
                    {
                        sel_vector->SetIndex(i, i);
                    }
                }

                auto new_sel_vector = std::make_shared<SelectionVector>(chunk.GetCapacity());

                if (compiled_batch_func_)
                {
                    // === JIT Batch Execution (SIMD Auto-Vectorized) ===
                    std::cout << "Using JIT Batch Execution" << std::endl;
                    std::vector<const int32_t *> cols;
                    cols.reserve(chunk.GetColumnCount());
                    for (size_t col_idx = 0; col_idx < chunk.GetColumnCount(); ++col_idx)
                    {
                        auto flat_vec = std::dynamic_pointer_cast<FlatVector<int32_t>>(chunk.GetVector(col_idx));
                        if (flat_vec)
                        {
                            cols.push_back(flat_vec->Data());
                        }
                        else
                        {
                            cols.push_back(nullptr);
                        }
                    }

                    std::vector<int32_t> results(chunk.GetCount());
                    std::vector<uint8_t> nulls(chunk.GetCount(), 0);
                    compiled_batch_func_((const void **)cols.data(), results.data(), nulls.data(), chunk.GetCount());

                    for (size_t i = 0; i < chunk.GetCount(); ++i)
                    {
                        size_t physical_idx = sel_vector->GetIndex(i);
                        if (results[i] != 0)
                        {
                            new_sel_vector->SetIndex(matched_count++, physical_idx);
                        }
                    }
                }
                else
                {
                    // === Volcano Batch Execution ===
                    std::cout << "Using Volcano Batch Execution" << std::endl;
                    std::shared_ptr<Vector> eval_result = std::make_shared<FlatVector<int32_t>>(TypeId::INTEGER, chunk.GetCapacity());
                    filter_expr->Evaluate(chunk, eval_result);

                    for (size_t i = 0; i < chunk.GetCount(); ++i)
                    {
                        size_t physical_idx = sel_vector->GetIndex(i);
                        // Using flat vector directly for fallback if we want, but GetValue is safer
                        Value val = eval_result->GetValue(physical_idx);
                        if (!val.IsNull() && val.GetAsInteger() != 0)
                        {
                            new_sel_vector->SetIndex(matched_count++, physical_idx);
                        }
                    }
                }

                if (matched_count > 0)
                {
                    chunk.SetCount(matched_count);
                    chunk.SetSelectionVector(new_sel_vector);
                    return true;
                }

                chunk.Reset();
            }

            return false;
        }

    private:
        const FilterPlanNode *plan_;
        std::unique_ptr<AbstractExecutor> child_executor_;

        JitEngine jit_engine_;
        JitEngine::CompiledExpressionFunc compiled_func_{nullptr};
        JitEngine::CompiledBatchFunc compiled_batch_func_{nullptr};
    };
} // namespace Database