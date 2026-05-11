#pragma once

#include <string>
#include <memory>
#include <vector>
#include "../Index/BPlusTree.h"
#include "../Index/GenericKey.h"
#include "../Common/RID.h"

namespace Database
{
    using IndexKeyType = GenericKey<64>;
    using IndexValueType = RID;

    struct IndexComparator
    {
        int operator()(const IndexKeyType &lhs, const IndexKeyType &rhs) const
        {
            if (lhs < rhs)
                return -1;
            if (lhs > rhs)
                return 1;
            return 0;
        }
    };

    using BPlusTreeIndex = BPlusTree<IndexKeyType, IndexValueType, IndexComparator>;

    struct IndexInfo
    {
        std::string index_name_;
        std::string table_name_;
        std::string column_name_;
        int column_idx_;
        std::unique_ptr<BPlusTreeIndex> btree_;

        IndexInfo(std::string index_name, std::string table_name, std::string column_name, int column_idx, std::unique_ptr<BPlusTreeIndex> btree)
            : index_name_(std::move(index_name)),
              table_name_(std::move(table_name)),
              column_name_(std::move(column_name)),
              column_idx_(column_idx),
              btree_(std::move(btree)) {}
    };

} // namespace Database