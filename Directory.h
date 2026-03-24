#pragma once

#include <vector>
#include <map>
#include <string>

#include "../Page.h"
#include "../BufferPoolManager.h"

// Simple catalog managing mapping from table_name -> root_page_id
class Directory
{
public:
    Directory(BufferPoolManager *bpm) : bpm_(bpm)
    {
        // Assume Page 0 is the directory page
        // For simplicity, we just keep it in memory map for now,
        // In real system, we serialize this map to Page 0.

        // Fetch page 0 to initialize if not exists
        page_id_t page_id;
        if (bpm_->FetchPage(0) == nullptr)
        {
            bpm_->NewPage(&page_id); // Create page 0
        }
    }

    void CreateTable(const std::string &table_name, page_id_t root_id)
    {
        table_map_[table_name] = root_id;
        // In real system: Serialize map to Page 0
    }

    page_id_t GetTableRoot(const std::string &table_name)
    {
        if (table_map_.find(table_name) != table_map_.end())
        {
            return table_map_[table_name];
        }
        return INVALID_PAGE_ID;
    }

private:
    BufferPoolManager *bpm_;
    std::map<std::string, page_id_t> table_map_;
};
