#pragma once

#include "BPlusTreePage.h"
#include <algorithm>
#include <vector>

// Forward declaration
class BufferPoolManager;

#define LEAF_PAGE_HEADER_SIZE 28
#define LEAF_PAGE_KEY_VALUE_PAIR_SIZE (sizeof(KeyType) + sizeof(ValueType))

/**
 * Store indexed key and record id(record id = page id slot id) - simplified to int
 * Leaf page format (keys are sorted):
 *  ----------------------------------------------------------------------
 * | Header | Key(1) | Value(1) | Key(2) | Value(2) | ... | Key(n) | Value(n)
 *  ----------------------------------------------------------------------
 */
class BPlusTreeLeafPage : public BPlusTreePage
{
public:
    void Init(page_id_t page_id, page_id_t parent_id = INVALID_PAGE_ID, int max_size = 0)
    {
        SetPageType(IndexPageType::LEAF_PAGE);
        SetSize(0);
        SetMaxSize(max_size);
        SetParentPageId(parent_id);
        SetPageId(page_id);
        next_page_id_ = INVALID_PAGE_ID;
    }

    page_id_t GetNextPageId() const { return next_page_id_; }
    void SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

    KeyType KeyAt(int index) const
    {
        return array_[index].first;
    }

    ValueType ValueAt(int index) const
    {
        return array_[index].second;
    }

    // Insert key & value pair (sorted)
    bool Insert(const KeyType &key, const ValueType &value, BufferPoolManager *bpm)
    {
        // Find insertion point
        int size = GetSize();
        if (size >= GetMaxSize())
        {
            return false; // Should split
        }

        int index = lower_bound(key); // Assuming key unique

        // Shift elements
        for (int i = size; i > index; i--)
        {
            array_[i] = array_[i - 1];
        }
        array_[index] = {key, value};
        IncreaseSize(1);
        return true;
    }

    // Split this page and move half to new page
    void MoveHalfTo(BPlusTreeLeafPage *recipient, BufferPoolManager *bpm)
    {
        // Typically split at middle
        int split_index = GetMaxSize() / 2;
        int original_size = GetSize();
        int move_count = original_size - split_index;

        recipient->CopyNFrom(array_ + split_index, move_count);

        SetSize(split_index);
        recipient->SetNextPageId(GetNextPageId());
        SetNextPageId(recipient->GetPageId());
    }

    void CopyNFrom(std::pair<KeyType, ValueType> *items, int size)
    {
        for (int i = 0; i < size; i++)
        {
            array_[GetSize() + i] = items[i];
        }
        IncreaseSize(size);
    }

    int lower_bound(const KeyType &key) const
    {
        // Linear scan
        for (int i = 0; i < GetSize(); i++)
        {
            if (array_[i].first >= key)
            {
                return i;
            }
        }
        return GetSize();
    }

    bool Lookup(const KeyType &key, ValueType &value, const KeyType &comparator) const
    {
        for (int i = 0; i < GetSize(); i++)
        {
            if (array_[i].first == key)
            {
                value = array_[i].second;
                return true;
            }
        }
        return false;
    }

private:
    page_id_t next_page_id_;
    std::pair<KeyType, ValueType> array_[1]; // Flexible array member
};
