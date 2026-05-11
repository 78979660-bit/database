#pragma once

#include "AbstractExecutor.h"
#include "../Plans/IndexScanPlanNode.h"
#include "../../Catalog/IndexInfo.h"
#include "../../Storage/Table/ColumnarTable.h"
#include <vector>

namespace Database
{

    class IndexScanExecutor : public AbstractExecutor
    {
    public:
        IndexScanExecutor(ExecutorContext *exec_ctx, const IndexScanPlanNode *plan)
            : AbstractExecutor(exec_ctx), plan_(plan) {}

        void Init() override
        {
            rids_.clear();
            idx_ = 0;

            // 1. 获取索引元数据和表元数据
            index_info_ = exec_ctx_->GetCatalog()->GetIndex(plan_->GetIndexName());
            if (index_info_ == nullptr)
                return;

            table_metadata_ = exec_ctx_->GetCatalog()->GetTable(index_info_->table_name_);
            if (table_metadata_ == nullptr)
                return;

            // 2. 将查询的 Value 转为 B+ Tree 使用的 GenericKey
            IndexKeyType search_key;
            Value pred_val = plan_->GetPredicateValue();
            search_key.SetString(pred_val.GetAsVarchar().c_str());

            // 3. 在 B+ 树中进行点查 (Point Query)
            index_info_->btree_->GetValue(search_key, rids_);
        }

        bool Next(Tuple *tuple, RID *rid) override
        {
            if (table_metadata_ == nullptr || idx_ >= rids_.size())
            {
                return false;
            }

            *rid = rids_[idx_++];

            // 从底层的 ColumnarTable 根据 RID 获取具体的记录数据
            bool result = table_metadata_->table_->GetTuple(*rid, tuple);

            if (!result)
            {
                // 如果获取失败（可能该版本被彻底清理），继续看下一个
                return Next(tuple, rid);
            }

            return true;
        }

        // Vectorized Next
        bool Next(Chunk &chunk) override
        {
            if (table_metadata_ == nullptr || idx_ >= rids_.size())
            {
                return false;
            }

            size_t output_count = 0;
            const size_t batch_size = chunk.GetCapacity();

            while (idx_ < rids_.size() && output_count < batch_size)
            {
                RID rid = rids_[idx_++];
                Tuple tuple;

                if (table_metadata_->table_->GetTuple(rid, &tuple))
                {
                    const Schema *schema = plan_->OutputSchema().get();
                    if (!schema)
                    {
                        schema = &table_metadata_->schema_;
                    }

                    for (size_t col_idx = 0; col_idx < schema->GetColumnCount(); ++col_idx)
                    {
                        Value val = tuple.GetValue(schema, col_idx);
                        chunk.GetVector(col_idx)->SetValue(output_count, val);
                    }

                    chunk.SetRID(output_count, rid);

                    output_count++;
                }
            }

            chunk.SetCount(output_count);
            return output_count > 0;
        }

    private:
        const IndexScanPlanNode *plan_;
        IndexInfo *index_info_{nullptr};
        TableMetadata *table_metadata_{nullptr};

        std::vector<RID> rids_; // 命中索引的所有物理行指针
        size_t idx_{0};
    };

} // namespace Database