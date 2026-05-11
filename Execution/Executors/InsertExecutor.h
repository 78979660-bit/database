#pragma once

#include "AbstractExecutor.h"
#include "../../Concurrency/Transaction.h"
#include "../Plans/InsertPlanNode.h"
#include "../../Storage/Table/ColumnarTable.h"
#include "../../Storage/ColumnStoreTable.h"
#include "../../Catalog/IndexInfo.h"

namespace Database
{

    class InsertExecutor : public AbstractExecutor
    {
    public:
        InsertExecutor(ExecutorContext *exec_ctx, const InsertPlanNode *plan,
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
                column_table_ = metadata->column_table_.get();
            }
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            if (finished_)
                return false;

            Tuple child_tuple;
            RID child_rid;
            int32_t count = 0;

            while (child_executor_->Next(&child_tuple, &child_rid))
            {
                RID inserted_rid;

                Transaction *txn = exec_ctx_->GetTransaction();

                TupleMeta meta;
                if (txn != nullptr)
                {
                    meta.insert_txn_id_ = txn->GetTransactionId();
                    meta.delete_txn_id_ = INVALID_TXN_ID;
                    meta.is_deleted_ = false;
                }

                if (columnar_table_ && columnar_table_->InsertTuple(meta, child_tuple, &inserted_rid))
                {
                    if (txn != nullptr)
                    {
                        // ��¼��������־������β��뱣�浽д����
                        txn->AppendWriteRecord({inserted_rid, WType::W_INSERT, columnar_table_});
                    }
                    if (column_table_)
                    {
                        std::vector<Value> vals;
                        for (uint32_t c = 0; c < table_info_->schema_.GetColumnCount(); ++c)
                        {
                            vals.push_back(child_tuple.GetValue(&table_info_->schema_, c));
                        }
                        column_table_->InsertTuple(vals);
                    } // ======== 维护索引 ========
                    auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(plan_->GetTableName());
                    for (auto index_info : indexes)
                    {
                        Value val = child_tuple.GetValue(&table_info_->schema_, index_info->column_idx_);
                        IndexKeyType key;
                        key.SetString(val.GetAsVarchar().c_str());
                        index_info->btree_->Insert(key, inserted_rid);
                    }
                    count++;
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

            int32_t count = 0;
            Chunk child_chunk(chunk.GetCapacity()); // Create temporary generic chunk for child

            // In actual columnar implementation, the engine pre-allocates vectors for child_chunk based on schema
            const Schema *child_schema = plan_->GetChildPlan()->GetOutputSchema();
            if (!child_schema)
            {
                // If the child didn't provide a schema (like a loosely bound Values node),
                // use the target table schema as the expected output from the child
                child_schema = &table_info_->schema_;
            }

            if (child_schema)
            {
                for (uint32_t i = 0; i < child_schema->GetColumnCount(); ++i)
                {
                    TypeId type = child_schema->GetColumn(i).GetType();
                    if (type == TypeId::INTEGER)
                        child_chunk.AddVector(std::make_shared<FlatVector<int32_t>>(type, child_chunk.GetCapacity()));
                    else if (type == TypeId::VARCHAR)
                        child_chunk.AddVector(std::make_shared<FlatVector<std::string>>(type, child_chunk.GetCapacity()));
                    else
                        child_chunk.AddVector(std::make_shared<FlatVector<int32_t>>(type, child_chunk.GetCapacity()));
                }
            }

            Transaction *txn = exec_ctx_->GetTransaction();

            TupleMeta meta;
            if (txn != nullptr)
            {
                meta.insert_txn_id_ = txn->GetTransactionId();
                meta.delete_txn_id_ = INVALID_TXN_ID;
                meta.is_deleted_ = false;
            }

            while (child_executor_->Next(child_chunk))
            {
                auto schema = plan_->GetChildPlan()->GetOutputSchema();
                auto sel_vector = child_chunk.GetSelectionVector();

                for (size_t i = 0; i < child_chunk.GetCount(); ++i)
                {
                    size_t physical_idx = sel_vector ? sel_vector->GetIndex(i) : i;

                    // Reconstruct tuple (for old ColumnarTable compatibility)
                    std::vector<Value> row_values;
                    for (size_t col = 0; col < child_chunk.GetColumnCount(); ++col)
                    {
                        row_values.push_back(child_chunk.GetVector(col)->GetValue(physical_idx));
                    }
                    Tuple child_tuple(row_values, schema);

                    RID inserted_rid;
                    if ((columnar_table_ && columnar_table_->InsertTuple(meta, child_tuple, &inserted_rid)) || column_table_)
                    {
                        if (column_table_)
                        {
                            column_table_->InsertTuple(row_values);
                            // Set dummy rid if solely columnar
                            if (!columnar_table_)
                                inserted_rid = RID(0, 0);
                        }

                        if (columnar_table_ && txn != nullptr)
                        {
                            txn->AppendWriteRecord({inserted_rid, WType::W_INSERT, columnar_table_});
                        }

                        auto indexes = exec_ctx_->GetCatalog()->GetTableIndexes(plan_->GetTableName());
                        for (auto index_info : indexes)
                        {
                            Value val = child_tuple.GetValue(&table_info_->schema_, index_info->column_idx_);
                            IndexKeyType key;
                            key.SetString(val.GetAsVarchar().c_str());
                            index_info->btree_->Insert(key, inserted_rid);
                        }
                        count++;
                    }
                }
                child_chunk.Reset();
            }

            finished_ = true;

            // Note: Returning integer count of inserted rows in chunk format is a standard way.
            // Assuming the insert plan output schema has 1 int column.
            if (chunk.GetColumnCount() > 0)
            {
                chunk.GetVector(0)->SetValue(0, Value(count));
                chunk.SetCount(1);
                return true;
            }
            return false;
        }

    private:
        const InsertPlanNode *plan_;
        std::unique_ptr<AbstractExecutor> child_executor_;
        TableMetadata *table_info_ = nullptr;
        ColumnarTable *columnar_table_ = nullptr;
        ColumnStoreTable *column_table_ = nullptr;
        bool finished_ = false;
    };

} // namespace Database
