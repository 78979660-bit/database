#pragma once
#include "BPlusTreeLeafPage.h"
#include "../BufferPoolManager.h"
#include <utility>

#define INDEXITERATOR_TYPE IndexIterator<KeyType, ValueType, KeyComparator>

template <typename KeyType, typename ValueType, typename KeyComparator>
class IndexIterator
{
public:
    // Constructor
    IndexIterator(BufferPoolManager *buffer_pool_manager, Page *page, int index);

    // Destructor
    ~IndexIterator();

    // Copy Constructor
    IndexIterator(const IndexIterator &other);

    // Assignment Operator
    IndexIterator &operator=(const IndexIterator &other);

    bool isEnd();

    const std::pair<KeyType, ValueType> &operator*();

    IndexIterator &operator++();

    bool operator==(const IndexIterator &itr) const
    {
        // Both are end
        if (page_ == nullptr && itr.page_ == nullptr)
        {
            return true;
        }
        // One is end
        if (page_ == nullptr || itr.page_ == nullptr)
        {
            return false;
        }
        return page_->GetPageId() == itr.page_->GetPageId() && index_ == itr.index_;
    }

    bool operator!=(const IndexIterator &itr) const
    {
        return !(*this == itr);
    }

private:
    BufferPoolManager *buffer_pool_manager_;
    Page *page_;
    BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *leaf_;
    int index_;
};

// Implementation

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator>::IndexIterator(BufferPoolManager *buffer_pool_manager, Page *page, int index)
    : buffer_pool_manager_(buffer_pool_manager), page_(page), index_(index)
{
    if (page_ != nullptr)
    {
        leaf_ = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(page_->GetData());
        // Normalize location if index is at the end of the page
        if (index_ >= leaf_->GetSize())
        {
            page_id_t next_page_id = leaf_->GetNextPageId();
            // Unpin current page as we might move or invalidate
            page_->RUnlatch();
            buffer_pool_manager_->UnpinPage(page_->GetPageId(), false);

            if (next_page_id != INVALID_PAGE_ID)
            {
                page_ = buffer_pool_manager_->FetchPage(next_page_id);
                if (page_ != nullptr)
                {
                    page_->RLatch();
                    leaf_ = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(page_->GetData());
                    index_ = 0;
                }
                else
                {
                    // Should not happen, but safety
                    leaf_ = nullptr;
                    index_ = 0;
                }
            }
            else
            {
                // End of Index
                page_ = nullptr;
                leaf_ = nullptr;
                index_ = 0;
            }
        }
    }
    else
    {
        leaf_ = nullptr;
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator>::~IndexIterator()
{
    if (page_ != nullptr)
    {
        page_->RUnlatch();
        buffer_pool_manager_->UnpinPage(page_->GetPageId(), false);
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator>::IndexIterator(const IndexIterator &other)
    : buffer_pool_manager_(other.buffer_pool_manager_), page_(other.page_), leaf_(other.leaf_), index_(other.index_)
{
    if (page_ != nullptr)
    {
        buffer_pool_manager_->FetchPage(page_->GetPageId());
        page_->RLatch();
    }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator> &IndexIterator<KeyType, ValueType, KeyComparator>::operator=(const IndexIterator &other)
{
    if (this != &other)
    {
        if (page_ != nullptr)
        {
            page_->RUnlatch();
            buffer_pool_manager_->UnpinPage(page_->GetPageId(), false);
        }
        buffer_pool_manager_ = other.buffer_pool_manager_;
        page_ = other.page_;
        leaf_ = other.leaf_;
        index_ = other.index_;
        if (page_ != nullptr)
        {
            buffer_pool_manager_->FetchPage(page_->GetPageId());
            page_->RLatch();
        }
    }
    return *this;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool IndexIterator<KeyType, ValueType, KeyComparator>::isEnd()
{
    return page_ == nullptr;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
const std::pair<KeyType, ValueType> &IndexIterator<KeyType, ValueType, KeyComparator>::operator*()
{
    return leaf_->GetItem(index_);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
IndexIterator<KeyType, ValueType, KeyComparator> &IndexIterator<KeyType, ValueType, KeyComparator>::operator++()
{
    if (page_ == nullptr)
        return *this;

    index_++;
    // If index reaches size, move to next page
    if (index_ >= leaf_->GetSize())
    {
        page_id_t next_page_id = leaf_->GetNextPageId();

        // Unpin current page
        page_->RUnlatch();
        buffer_pool_manager_->UnpinPage(page_->GetPageId(), false);

        if (next_page_id == INVALID_PAGE_ID || next_page_id == 0) // Check 0 as well for safety
        {
            page_ = nullptr;
            leaf_ = nullptr;
            index_ = 0;
        }
        else
        {
            page_ = buffer_pool_manager_->FetchPage(next_page_id);
            if (page_ == nullptr)
            {
                leaf_ = nullptr;
                index_ = 0;
            }
            else
            {
                page_->RLatch();
                leaf_ = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(page_->GetData());
                index_ = 0;
            }
        }
    }
    return *this;
}
