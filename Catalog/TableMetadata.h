#pragma once

#include <string>
#include <memory>
#include "Schema.h"
#include "TableStatistics.h"
#include "../Storage/Table/ColumnarTable.h"
#include "../Storage/ColumnStoreTable.h"
#include "../Storage/Table/ColumnarTable.h"

namespace Database
{
    struct TableMetadata
    {
        TableMetadata(Schema schema, std::string name, page_id_t root_page_id, table_oid_t oid, std::unique_ptr<ColumnStoreTable> column_table = nullptr)
            : schema_(std::move(schema)), name_(std::move(name)), column_table_(std::move(column_table)), root_page_id_(root_page_id), oid_(oid)
        {
            // [系统级真列存集成点] 为这个表直接创建一个纯列态备用结构！
            columnar_storage_ = std::make_unique<ColumnarTable>(&schema_);
            stats_.tuple_count_ = 10000;
            stats_.page_count_ = 100;
        }

        Schema schema_;
        std::string name_;
        std::unique_ptr<ColumnarTable> table_;
        std::unique_ptr<ColumnStoreTable> column_table_;
        // 新时代真·列存储，彻底代替 ColumnarTable/SlottedPage 物理层存储结构
        std::unique_ptr<ColumnarTable> columnar_storage_;

        page_id_t root_page_id_;
        table_oid_t oid_;

        // Data distribution statistics for the cost model
        TableStatistics stats_;
    };

} // namespace Database