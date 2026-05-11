#pragma once

#include "BPlusTreePage.h"
#include <algorithm>
#include <vector>

// Forward declaration
class BufferPoolManager;

#define LEAF_PAGE_HEADER_SIZE 28

/**
 * Store indexed key and record id(record id = page id slot id) - simplified to int
 * Leaf page format (keys are sorted):
 *  ----------------------------------------------------------------------
 * | Header | Key(1) | Value(1) | Key(2) | Value(2) | ... | Key(n) | Value(n)
 *  ----------------------------------------------------------------------
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
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

    const std::pair<KeyType, ValueType> &GetItem(int index) const
    {
        return array_[index];
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

        int index = lower_bound(key);

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

    // Move all elements to recipient
    void MoveAllTo(BPlusTreeLeafPage *recipient)
    {
        recipient->CopyNFrom(array_, GetSize());
        recipient->SetNextPageId(GetNextPageId());
        SetSize(0);
    }

    // Move first to end of recipient
    void MoveFirstToEndOf(BPlusTreeLeafPage *recipient)
    {
        recipient->CopyLastFrom(array_[0]);
        // Shift remaining
        for (int i = 0; i < GetSize() - 1; i++)
        {
            array_[i] = array_[i + 1];
        }
        IncreaseSize(-1);
    }

    // Move last to front of recipient
    void MoveLastToFrontOf(BPlusTreeLeafPage *recipient)
    {
        recipient->CopyFirstFrom(array_[GetSize() - 1]);
        IncreaseSize(-1);
    }

    void CopyLastFrom(const std::pair<KeyType, ValueType> &item)
    {
        array_[GetSize()] = item;
        IncreaseSize(1);
    }

    void CopyFirstFrom(const std::pair<KeyType, ValueType> &item)
    {
        for (int i = GetSize(); i > 0; i--)
        {
            array_[i] = array_[i - 1];
        }
        array_[0] = item;
        IncreaseSize(1);
    }

    int lower_bound(const KeyType &key) const
    {
        // Linear scan using operator>=
        for (int i = 0; i < GetSize(); i++)
        {
            if (array_[i].first >= key)
            {
                return i;
            }
        }
        return GetSize();
    }

    // Remove key & value pair with given key (return size after remove)
    int RemoveAndDeleteRecord(const KeyType &key, const KeyComparator &comparator)
    {
        int index = KeyIndex(key, comparator);
        if (index < GetSize() && comparator(array_[index].first, key) == 0)
        {
            for (int i = index; i < GetSize() - 1; ++i)
            {
                array_[i] = array_[i + 1];
            }
            IncreaseSize(-1);
            return GetSize();
        }
        return GetSize();
    }

    int KeyIndex(const KeyType &key, const KeyComparator &comparator) const
    {
        for (int i = 0; i < GetSize(); i++)
        {
            if (comparator(key, array_[i].first) == 0)
                return i;
        }
        return GetSize();
    }

    bool Lookup(const KeyType &key, ValueType &value, const KeyComparator &comparator) const
    {
        for (int i = 0; i < GetSize(); i++)
        {
            if (comparator(key, array_[i].first) == 0)
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
