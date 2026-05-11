#pragma once

#include "AbstractExecutor.h"
#include "../../Concurrency/Transaction.h"
#include "../Plans/DeletePlanNode.h"
#include "../../Storage/Table/ColumnarTable.h"
#include "../../Catalog/IndexInfo.h"

namespace Database
{

    class DeleteExecutor : public AbstractExecutor
    {
    public:
        DeleteExecutor(ExecutorContext *exec_ctx, const DeletePlanNode *plan,
                       std::unique_ptr<AbstractExecutor> &&child_executor)
            : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

        void Init() override
        {
            if (child_executor_)
                child_executor_->Init();

            TableMetadata *metadata = exec_ctx_->GetCatalog()->GetTable(plan_->GetTableName());
            if (metadata)
            {
                table_info_ = metadata;
                columnar_table_ = metadata->columnar_storage_.get();
            }
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            if (finished_)
                return false;

            Tuple child_tuple;
            RID child_rid;
            int32_t delete_count = 0;

            Transaction *txn = exec_ctx_->GetTransaction();
            LockManager *lock_manager = exec_ctx_->GetLockManager();

            while (child_executor_->Next(&child_tuple, &child_rid))
            {
                if (columnar_table_ && txn != nullptr)
                {
                    // Acquire Exclusive lock before modification
                    if (lock_manager != nullptr)
                    {
                        if (!lock_manager->LockExclusive(txn, child_rid))
                        {
                            // Transaction ABORTED
                            return false;
                        }
                    }

                    TupleMeta old_meta = child_tuple.GetMeta();

                    if (old_meta.delete_txn_id_ == INVALID_TXN_ID)
                    {
                        old_meta.delete_txn_id_ = txn->GetTransactionId();

                        if (columnar_table_->UpdateTupleMeta(old_meta, child_rid))
                        {
                            txn->AppendWriteRecord({child_rid, WType::W_DELETE, columnar_table_});
                            // ======== 维护索引 ========
                            auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(plan_->GetTableName());
                            for (auto index_info : indexes)
                            {
                                Value val = child_tuple.GetValue(&table_info_->schema_, index_info->column_idx_);
                                IndexKeyType key;
                                key.SetString(val.GetAsVarchar().c_str());
                                index_info->btree_->Remove(key);
                            }
                            delete_count++;
                        }
                    }
                }
            }

            finished_ = true;
            return false;
        }

        // Vectorized Next
        bool Next(Chunk &chunk) override
        {
            if (finished_)
                return false;

            int32_t delete_count = 0;
            Chunk child_chunk(chunk.GetCapacity());

            Transaction *txn = exec_ctx_->GetTransaction();
            LockManager *lock_manager = exec_ctx_->GetLockManager();

            while (child_executor_->Next(child_chunk))
            {
                if (!columnar_table_ || txn == nullptr)
                {
                    child_chunk.Reset();
                    continue;
                }

                auto schema = plan_->GetChildPlan()->GetOutputSchema();
                auto sel_vector = child_chunk.GetSelectionVector();

                for (size_t i = 0; i < child_chunk.GetCount(); ++i)
                {
                    size_t physical_idx = sel_vector ? sel_vector->GetIndex(i) : i;

                    RID child_rid = child_chunk.GetRID(physical_idx);

                    if (lock_manager != nullptr)
                    {
                        if (!lock_manager->LockExclusive(txn, child_rid))
                        {
                            return false;
                        }
                    }

                    // Needs the old tuple for index deletion etc
                    Tuple child_tuple;
                    if (columnar_table_->GetTuple(child_rid, &child_tuple))
                    {
                        TupleMeta old_meta = child_tuple.GetMeta();

                        if (old_meta.delete_txn_id_ == INVALID_TXN_ID)
                        {
                            old_meta.delete_txn_id_ = txn->GetTransactionId();

                            if (columnar_table_->UpdateTupleMeta(old_meta, child_rid))
                            {
                                txn->AppendWriteRecord({child_rid, WType::W_DELETE, columnar_table_});
                                auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(plan_->GetTableName());
                                for (auto index_info : indexes)
                                {
                                    Value val = child_tuple.GetValue(&table_info_->schema_, index_info->column_idx_);
                                    IndexKeyType key;
                                    key.SetString(val.GetAsVarchar().c_str());
                                    index_info->btree_->Remove(key);
                                }
                                delete_count++;
                            }
                        }
                    }
                }
                child_chunk.Reset();
            }

            finished_ = true;

            if (chunk.GetColumnCount() > 0)
            {
                chunk.GetVector(0)->SetValue(0, Value(delete_count));
                chunk.SetCount(1);
                return true;
            }
            return false;
        }

    private:
        const DeletePlanNode *plan_;
        std::unique_ptr<AbstractExecutor> child_executor_;
        TableMetadata *table_info_ = nullptr;
        ColumnarTable *columnar_table_ = nullptr;
        bool finished_ = false;
    };

} // namespace Database
