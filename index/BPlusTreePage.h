#pragma once

#include <string>
#include <vector>
#include <cstring>
#include "../Page.h"

// Define index keys and values for simplicity (int for now)
using KeyType = int;
using ValueType = int;
#define INVALID_PAGE_ID -1

enum class IndexPageType
{
    INVALID_INDEX_PAGE = 0,
    LEAF_PAGE,
    INTERNAL_PAGE
};

// Abstract base class for B+ Tree pages
// Header format: [PageType (4)] [LSN (4)] [CurrentSize (4)] [MaxSize (4)] [ParentPageId (4)] [PageId (4)]
// Total header size: 24 bytes
class BPlusTreePage
{
public:
    bool IsLeafPage() const { return page_type_ == IndexPageType::LEAF_PAGE; }
    void SetPageType(IndexPageType page_type) { page_type_ = page_type; }

    int GetSize() const { return size_; }
    void SetSize(int size) { size_ = size; }
    void IncreaseSize(int amount) { size_ += amount; }

    int GetMaxSize() const { return max_size_; }
    void SetMaxSize(int max_size) { max_size_ = max_size; }

    int GetMinSize() const
    {
        if (IsLeafPage())
        {
            return max_size_ / 2;
        }
        return (max_size_ + 1) / 2;
    }

    page_id_t GetParentPageId() const { return parent_page_id_; }
    void SetParentPageId(page_id_t parent_page_id) { parent_page_id_ = parent_page_id; }

    bool IsRootPage() const { return parent_page_id_ == INVALID_PAGE_ID; }

    page_id_t GetPageId() const { return page_id_; }
    void SetPageId(page_id_t page_id) { page_id_ = page_id; }

    void SetLSN(lsn_t lsn) { lsn_ = lsn; }
    lsn_t GetLSN() const { return lsn_; }

private:
    // These members reflect the memory layout on the page.
    // We cast the char[] data buffer to this struct.
    IndexPageType page_type_;
    lsn_t lsn_;
    int size_;
    int max_size_;
    page_id_t parent_page_id_;
    page_id_t page_id_;
};
