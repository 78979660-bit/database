#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "../BufferPoolManager.h"
#include "Schema.h"
#include "TableMetadata.h"
#include "IndexInfo.h"

namespace Database
{

    class Catalog
    {
    private:
        table_oid_t next_table_oid_{0};
        BufferPoolManager *bpm_;
        std::unordered_map<std::string, std::unique_ptr<TableMetadata>> tables_;
        std::unordered_map<std::string, std::unique_ptr<IndexInfo>> indexes_;
        std::unordered_map<std::string, std::vector<IndexInfo *>> table_indexes_;

    public:
        Catalog(BufferPoolManager *bpm) : bpm_(bpm) {}

        TableMetadata *CreateTable(Transaction *txn, const std::string &table_name, const Schema &schema)
        {
            if (tables_.find(table_name) != tables_.end())
            {
                return nullptr;
            }

            table_oid_t oid = next_table_oid_++;

            auto metadata = std::make_unique<TableMetadata>(schema, table_name, INVALID_PAGE_ID, oid);
            TableMetadata *ptr = metadata.get();
            tables_[table_name] = std::move(metadata);

            return ptr;
        }

        TableMetadata *GetTable(const std::string &table_name)
        {
            if (tables_.find(table_name) == tables_.end())
            {
                return nullptr;
            }
            return tables_[table_name].get();
        }

        std::vector<std::string> GetAllTableNames() const
        {
            std::vector<std::string> names;
            for (const auto &pair : tables_)
            {
                names.push_back(pair.first);
            }
            return names;
        }

        IndexInfo *CreateIndex(const std::string &index_name, const std::string &table_name, const std::string &column_name)
        {
            if (indexes_.find(index_name) != indexes_.end())
                return nullptr;
            auto table_meta = GetTable(table_name);
            if (!table_meta)
                return nullptr;
            int col_idx = table_meta->schema_.GetColumnIndex(column_name);
            if (col_idx == -1)
                return nullptr;

            auto btree = std::make_unique<BPlusTreeIndex>(index_name, bpm_);
            auto index_info = std::make_unique<IndexInfo>(index_name, table_name, column_name, col_idx, std::move(btree));

            IndexInfo *ptr = index_info.get();
            indexes_[index_name] = std::move(index_info);
            table_indexes_[table_name].push_back(ptr);

            return ptr;
        }

        IndexInfo *GetIndex(const std::string &index_name)
        {
            if (indexes_.find(index_name) == indexes_.end())
                return nullptr;
            return indexes_[index_name].get();
        }

        std::vector<IndexInfo *> GetTableIndexes(const std::string &table_name)
        {
            if (table_indexes_.find(table_name) == table_indexes_.end())
                return {};
            return table_indexes_[table_name];
        }
    };

} // namespace Database
