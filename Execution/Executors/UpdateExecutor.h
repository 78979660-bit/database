#pragma once

#include "AbstractExecutor.h"
#include "../../Concurrency/Transaction.h"
#include "../Plans/UpdatePlanNode.h"
#include "../../Storage/Table/ColumnarTable.h"
#include "../../Catalog/IndexInfo.h"

namespace Database
{

    class UpdateExecutor : public AbstractExecutor
    {
    public:
        UpdateExecutor(ExecutorContext *exec_ctx, const UpdatePlanNode *plan,
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
            int32_t update_count = 0;

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
                        // ======== 1. �߼�ɾ���ɵ����� ========
                        old_meta.delete_txn_id_ = txn->GetTransactionId();
                        if (columnar_table_->UpdateTupleMeta(old_meta, child_rid))
                        {
                            txn->AppendWriteRecord({child_rid, WType::W_DELETE, columnar_table_});
                            // 维护索引：删除旧值
                            auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(plan_->GetTableName());
                            for (auto index_info : indexes)
                            {
                                Value old_val = child_tuple.GetValue(&table_info_->schema_, index_info->column_idx_);
                                IndexKeyType key;
                                key.SetString(old_val.GetAsVarchar().c_str());
                                index_info->btree_->Remove(key);
                            }
                        }

                        // ======== 2. �����µ����� ========
                        TupleMeta new_meta;
                        new_meta.insert_txn_id_ = txn->GetTransactionId();
                        new_meta.delete_txn_id_ = INVALID_TXN_ID;
                        new_meta.is_deleted_ = false;

                        // According to update plan, build new tuple values
                        std::vector<Value> new_values;
                        const Schema *schema = &table_info_->schema_;
                        const auto &update_attrs = plan_->GetUpdateAttrs();
                        for (uint32_t i = 0; i < schema->GetColumnCount(); ++i)
                        {
                            std::string col_name = schema->GetColumn(i).GetName();
                            auto it = update_attrs.find(col_name);
                            if (it != update_attrs.end())
                            {
                                new_values.push_back(it->second);
                            }
                            else
                            {
                                new_values.push_back(child_tuple.GetValue(schema, i));
                            }
                        }
                        Tuple new_tuple(new_values, schema);

                        RID new_rid;
                        if (columnar_table_->InsertTuple(new_meta, new_tuple, &new_rid))
                        {
                            txn->AppendWriteRecord({new_rid, WType::W_INSERT, columnar_table_});

                            // 维护索引：插入新值
                            auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(plan_->GetTableName());
                            for (auto index_info : indexes)
                            {
                                Value new_val = new_tuple.GetValue(&table_info_->schema_, index_info->column_idx_);
                                IndexKeyType key;
                                key.SetString(new_val.GetAsVarchar().c_str());
                                index_info->btree_->Insert(key, new_rid);
                            }

                            update_count++;
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

            int32_t update_count = 0;
            Chunk child_chunk(chunk.GetCapacity());

            Transaction *txn = exec_ctx_->GetTransaction();

            while (child_executor_->Next(child_chunk))
            {
                if (!columnar_table_ || txn == nullptr)
                {
                    child_chunk.Reset();
                    continue;
                }

                auto sel_vector = child_chunk.GetSelectionVector();

                for (size_t i = 0; i < child_chunk.GetCount(); ++i)
                {
                    size_t physical_idx = sel_vector ? sel_vector->GetIndex(i) : i;

                    RID child_rid = child_chunk.GetRID(physical_idx);

                    Tuple child_tuple;
                    if (columnar_table_->GetTuple(child_rid, &child_tuple))
                    {
                        TupleMeta old_meta = child_tuple.GetMeta();

                        if (old_meta.delete_txn_id_ == INVALID_TXN_ID)
                        {
                            // ======== 1. Logical Delete ========
                            old_meta.delete_txn_id_ = txn->GetTransactionId();
                            if (columnar_table_->UpdateTupleMeta(old_meta, child_rid))
                            {
                                txn->AppendWriteRecord({child_rid, WType::W_DELETE, columnar_table_});
                                auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(plan_->GetTableName());
                                for (auto index_info : indexes)
                                {
                                    Value old_val = child_tuple.GetValue(&table_info_->schema_, index_info->column_idx_);
                                    IndexKeyType key;
                                    key.SetString(old_val.GetAsVarchar().c_str());
                                    index_info->btree_->Remove(key);
                                }
                            }

                            // ======== 2. Insert New ========
                            TupleMeta new_meta;
                            new_meta.insert_txn_id_ = txn->GetTransactionId();
                            new_meta.delete_txn_id_ = INVALID_TXN_ID;
                            new_meta.is_deleted_ = false;

                            std::vector<Value> new_values;
                            const Schema *schema = &table_info_->schema_;
                            const auto &update_attrs = plan_->GetUpdateAttrs();
                            for (uint32_t c = 0; c < schema->GetColumnCount(); ++c)
                            {
                                std::string col_name = schema->GetColumn(c).GetName();
                                auto it = update_attrs.find(col_name);
                                if (it != update_attrs.end())
                                {
                                    new_values.push_back(it->second);
                                }
                                else
                                {
                                    new_values.push_back(child_tuple.GetValue(schema, c));
                                }
                            }
                            Tuple new_tuple(new_values, schema);

                            RID new_rid;
                            if (columnar_table_->InsertTuple(new_meta, new_tuple, &new_rid))
                            {
                                txn->AppendWriteRecord({new_rid, WType::W_INSERT, columnar_table_});
                                auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(plan_->GetTableName());
                                for (auto index_info : indexes)
                                {
                                    Value new_val = new_tuple.GetValue(&table_info_->schema_, index_info->column_idx_);
                                    IndexKeyType key;
                                    key.SetString(new_val.GetAsVarchar().c_str());
                                    index_info->btree_->Insert(key, new_rid);
                                }
                                update_count++;
                            }
                        }
                    }
                }
                child_chunk.Reset();
            }

            finished_ = true;

            if (chunk.GetColumnCount() > 0)
            {
                chunk.GetVector(0)->SetValue(0, Value(update_count));
                chunk.SetCount(1);
                return true;
            }
            return false;
        }

    private:
        const UpdatePlanNode *plan_;
        std::unique_ptr<AbstractExecutor> child_executor_;
        TableMetadata *table_info_ = nullptr;
        ColumnarTable *columnar_table_ = nullptr;
        bool finished_ = false;
    };

} // namespace Database
