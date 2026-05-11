#pragma once

#include "BPlusTreePage.h"
#include <algorithm>
#include <iostream>
#include "../BufferPoolManager.h"

#define INTERNAL_PAGE_HEADER_SIZE 24

/**
 * Store n indexed keys and n+1 child pointers (page_id) within internal page.
 * Pointer(0) < Key(1) < Pointer(1) < Key(2) < Pointer(2) ... < Key(n) < Pointer(n)
 * NOTE: The first key is always invalid.
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
class BPlusTreeInternalPage : public BPlusTreePage
{
public:
    // Initialize the internal page
    void Init(page_id_t page_id, page_id_t parent_id = INVALID_PAGE_ID, int max_size = 0)
    {
        SetPageType(IndexPageType::INTERNAL_PAGE);
        SetSize(0);
        SetMaxSize(max_size);
        SetParentPageId(parent_id);
        SetPageId(page_id);
    }

    KeyType KeyAt(int index) const
    {
        return array_[index].first;
    }

    void SetKeyAt(int index, const KeyType &key)
    {
        array_[index].first = key;
    }

    page_id_t ValueAt(int index) const
    {
        return array_[index].second;
    }

    void SetValueAt(int index, const page_id_t &value)
    {
        array_[index].second = value;
    }

    // Lookup the value for a given key, return the child page id
    // Finds the last key <= search_key
    page_id_t Lookup(const KeyType &key, const KeyComparator &comparator) const
    {
        // Linear scan for simplicity (Binary search is better)
        for (int i = 1; i < GetSize(); i++)
        {
            if (array_[i].first > key)
            {
                return array_[i - 1].second;
            }
        }
        return array_[GetSize() - 1].second;
    }

    // Populate new root with old_value + new_key & new_value
    void PopulateNewRoot(const page_id_t &old_value, const KeyType &new_key, const page_id_t &new_value)
    {
        SetSize(2);
        array_[0].second = old_value;
        array_[1].first = new_key;
        array_[1].second = new_value;
    }

    // Insert a new key & value pair into internal page
    int InsertNodeAfter(const page_id_t &old_value, const KeyType &new_key, const page_id_t &new_value)
    {
        int index = ValueIndex(old_value);
        // Move subsequent elements
        for (int i = GetSize(); i > index + 1; i--)
        {
            array_[i] = array_[i - 1];
        }
        array_[index + 1] = {new_key, new_value};
        IncreaseSize(1);
        return GetSize();
    }

    // Find index of a value
    int ValueIndex(const page_id_t &value) const
    {
        for (int i = 0; i < GetSize(); i++)
        {
            if (array_[i].second == value)
            {
                return i;
            }
        }
        return -1;
    }

    // Move all elements to 'recipient' which is at right
    void MoveAllTo(BPlusTreeInternalPage *recipient, const KeyType &middle_key, BufferPoolManager *bpm)
    {
        SetKeyAt(0, middle_key);
        recipient->CopyNFrom(array_, GetSize(), bpm);
        SetSize(0);
    }

    void MoveFirstToEndOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key, BufferPoolManager *bpm)
    {
        SetKeyAt(0, middle_key);
        recipient->CopyLastFrom(array_[0], bpm);

        for (int i = 0; i < GetSize() - 1; i++)
        {
            array_[i] = array_[i + 1];
        }
        IncreaseSize(-1);
    }

    void MoveLastToFrontOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key, BufferPoolManager *bpm)
    {
        recipient->SetKeyAt(0, middle_key);
        recipient->CopyFirstFrom(array_[GetSize() - 1], bpm);
        IncreaseSize(-1);
    }

    void CopyLastFrom(const std::pair<KeyType, page_id_t> &item, BufferPoolManager *bpm)
    {
        array_[GetSize()] = item;
        IncreaseSize(1);

        auto *child = bpm->FetchPage(item.second);
        auto *node = reinterpret_cast<BPlusTreePage *>(child->GetData());
        node->SetParentPageId(GetPageId());
        bpm->UnpinPage(item.second, true);
    }

    void CopyFirstFrom(const std::pair<KeyType, page_id_t> &item, BufferPoolManager *bpm)
    {
        for (int i = GetSize(); i > 0; i--)
        {
            array_[i] = array_[i - 1];
        }
        array_[0] = item;
        IncreaseSize(1);

        auto *child = bpm->FetchPage(item.second);
        auto *node = reinterpret_cast<BPlusTreePage *>(child->GetData());
        node->SetParentPageId(GetPageId());
        bpm->UnpinPage(item.second, true);
    }

    // Move half of key & value pairs from current page to recipient page
    void MoveHalfTo(BPlusTreeInternalPage *recipient, BufferPoolManager *bpm)
    {
        int start_idx = (GetSize() + 1) / 2; // Split point
        int original_size = GetSize();
        int move_count = original_size - start_idx;

        recipient->CopyNFrom(array_ + start_idx, move_count, bpm);
        SetSize(start_idx);
    }

    void CopyNFrom(std::pair<KeyType, page_id_t> *items, int size, BufferPoolManager *bpm)
    {
        for (int i = 0; i < size; i++)
        {
            array_[GetSize() + i] = items[i];

            // Update parent pointers of children
            page_id_t child_page_id = items[i].second;
            Page *child_page = bpm->FetchPage(child_page_id);
            if (child_page != nullptr)
            {
                BPlusTreePage *child_node = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
                child_node->SetParentPageId(GetPageId());
                bpm->UnpinPage(child_page_id, true);
            }
        }
        IncreaseSize(size);
    }

    // Remove the key & value pair in internal page according to input index(a.k.a array offset)
    void Remove(int index)
    {
        for (int i = index; i < GetSize() - 1; ++i)
        {
            array_[i] = array_[i + 1];
        }
        IncreaseSize(-1);
    }

private:
    // Flexible array member
    // First element is (invalid_key, value)
    std::pair<KeyType, page_id_t> array_[1];
};
