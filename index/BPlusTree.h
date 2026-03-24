#pragma once

#include <vector>
#include <iostream>
#include <string>
#include "BPlusTreePage.h"
#include "BPlusTreeLeafPage.h"
#include "BPlusTreeInternalPage.h"
#include "../Page.h"
#include "../BufferPoolManager.h"

// Forward declaration
class BPlusTreeLeafPage;
class BPlusTreeInternalPage;

/**
 * Main class providing the API for the Interactive B+ Tree.
 *
 * Implementation of simple B+ tree data structure where internal pages direct
 * the search and leaf pages contain actual data.
 * (1) We only support unique key
 * (2) support insert & remove
 * (3) The structure should shrink and grow dynamically
 * (4) Implement index iterator for range scan
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
class BPlusTree
{
public:
    explicit BPlusTree(std::string name, BufferPoolManager *buffer_pool_manager)
        : index_name_(std::move(name)),
          root_page_id_(INVALID_PAGE_ID),
          buffer_pool_manager_(buffer_pool_manager)
    {
        // Check if root page exists in header page (page 0 directory)
        // For now assume new
    }

    // Returns true if this B+ tree has no keys and values.
    bool IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }

    // Insert a key-value pair into this B+ tree.
    bool Insert(const KeyType &key, const ValueType &value);

    // Remove a key and its value from this B+ tree.
    void Remove(const KeyType &key) {} // Not implemented for now

    // Return the value associated with a given key
    bool GetValue(const KeyType &key, std::vector<ValueType> &result);

    // Return the page id of the root node
    page_id_t GetRootPageId() { return root_page_id_; }

private:
    void StartNewTree(const KeyType &key, const ValueType &value);
    bool InsertIntoLeaf(const KeyType &key, const ValueType &value);
    void InsertIntoParent(BPlusTreePage *old_node, const KeyType &key, BPlusTreePage *new_node);

    template <typename N>
    N *Split(N *node);

    BPlusTreePage *FetchPage(page_id_t page_id);
    BPlusTreeInternalPage *FetchInternalPage(page_id_t page_id);
    BPlusTreeLeafPage *FetchLeafPage(page_id_t page_id);

    Page *FindLeafPage(const KeyType &key, bool audit = false); // Returns raw page

    std::string index_name_;
    page_id_t root_page_id_;
    BufferPoolManager *buffer_pool_manager_;
    KeyComparator comparator_; // Unused in simplified int version
    int leaf_max_size_ = 4;    // Small for testing splits
    int internal_max_size_ = 5;
};

/*
 * HELPERS
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
BPlusTreePage *BPlusTree<KeyType, ValueType, KeyComparator>::FetchPage(page_id_t page_id)
{
    auto page = buffer_pool_manager_->FetchPage(page_id);
    return reinterpret_cast<BPlusTreePage *>(page->GetData());
}

template <typename KeyType, typename ValueType, typename KeyComparator>
BPlusTreeLeafPage *BPlusTree<KeyType, ValueType, KeyComparator>::FetchLeafPage(page_id_t page_id)
{
    auto page = buffer_pool_manager_->FetchPage(page_id);
    return reinterpret_cast<BPlusTreeLeafPage *>(page->GetData());
}

template <typename KeyType, typename ValueType, typename KeyComparator>
BPlusTreeInternalPage *BPlusTree<KeyType, ValueType, KeyComparator>::FetchInternalPage(page_id_t page_id)
{
    auto page = buffer_pool_manager_->FetchPage(page_id);
    return reinterpret_cast<BPlusTreeInternalPage *>(page->GetData());
}

/*
 * Implementation of search
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::GetValue(const KeyType &key, std::vector<ValueType> &result)
{
    if (IsEmpty())
    {
        return false;
    }

    // Find leaf page containing key
    Page *page = FindLeafPage(key);
    if (page == nullptr)
    {
        return false;
    }

    BPlusTreeLeafPage *leaf = reinterpret_cast<BPlusTreeLeafPage *>(page->GetData());
    ValueType val;
    bool found = leaf->Lookup(key, val, comparator_);
    if (found)
    {
        result.push_back(val);
    }

    // Remember to cast page back to Page class or retrieve page_id from BPlusTreePage
    // Assuming GetPageId works because BPlusTreePage layout matches data
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return found;
}

/*
 * Find the leaf page that contains the key
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
Page *BPlusTree<KeyType, ValueType, KeyComparator>::FindLeafPage(const KeyType &key, bool audit)
{
    // Audit log or something if needed
    if (root_page_id_ == INVALID_PAGE_ID)
    {
        return nullptr;
    }

    // Start from root
    Page *page = buffer_pool_manager_->FetchPage(root_page_id_);
    BPlusTreePage *node = reinterpret_cast<BPlusTreePage *>(page->GetData());

    while (!node->IsLeafPage())
    {
        BPlusTreeInternalPage *internal = reinterpret_cast<BPlusTreeInternalPage *>(node);
        page_id_t next_page_id = internal->Lookup(key, comparator_);

        // Unpin current, fetch next
        buffer_pool_manager_->UnpinPage(internal->GetPageId(), false);
        page = buffer_pool_manager_->FetchPage(next_page_id);
        node = reinterpret_cast<BPlusTreePage *>(page->GetData());
    }

    return page;
}

/*
 * Implementation of Insert
 */
template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::Insert(const KeyType &key, const ValueType &value)
{
    if (IsEmpty())
    {
        StartNewTree(key, value);
        return true;
    }
    return InsertIntoLeaf(key, value);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::StartNewTree(const KeyType &key, const ValueType &value)
{
    // Create new root (leaf)
    page_id_t page_id;
    Page *page = buffer_pool_manager_->NewPage(&page_id);

    if (page == nullptr)
    {
        throw std::runtime_error("Out of memory");
    }

    root_page_id_ = page_id;

    BPlusTreeLeafPage *root = reinterpret_cast<BPlusTreeLeafPage *>(page->GetData());
    root->Init(page_id, INVALID_PAGE_ID, leaf_max_size_);
    root->Insert(key, value, buffer_pool_manager_);

    buffer_pool_manager_->UnpinPage(page_id, true);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
bool BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoLeaf(const KeyType &key, const ValueType &value)
{
    // 1. Find leaf page
    Page *page = FindLeafPage(key);
    BPlusTreeLeafPage *leaf = reinterpret_cast<BPlusTreeLeafPage *>(page->GetData());

    // 2. Insert into leaf
    if (leaf->GetSize() < leaf->GetMaxSize())
    {
        leaf->Insert(key, value, buffer_pool_manager_);
        buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
        return true;
    }

    // 3. Split if full
    // Creating a new leaf page
    BPlusTreeLeafPage *new_leaf = Split(leaf);

    // Insert into split nodes
    if (key < new_leaf->KeyAt(0))
    {
        leaf->Insert(key, value, buffer_pool_manager_);
    }
    else
    {
        new_leaf->Insert(key, value, buffer_pool_manager_);
    }

    // Insert into parent
    InsertIntoParent(leaf, new_leaf->KeyAt(0), new_leaf);

    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(), true);

    return true;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
template <typename N>
N *BPlusTree<KeyType, ValueType, KeyComparator>::Split(N *node)
{
    page_id_t page_id;
    Page *page = buffer_pool_manager_->NewPage(&page_id);
    N *new_node = reinterpret_cast<N *>(page->GetData());

    // Init new node
    new_node->Init(page_id, node->GetParentPageId(), node->GetMaxSize());

    // Move half
    node->MoveHalfTo(new_node, buffer_pool_manager_);

    return new_node;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPlusTree<KeyType, ValueType, KeyComparator>::InsertIntoParent(BPlusTreePage *old_node, const KeyType &key, BPlusTreePage *new_node)
{
    if (old_node->IsRootPage())
    { // BPlusTreePage needs IsRootPage implementation or we check parent_id
        // Create new root (internal)
        page_id_t page_id;
        Page *page = buffer_pool_manager_->NewPage(&page_id);
        root_page_id_ = page_id;

        BPlusTreeInternalPage *root = reinterpret_cast<BPlusTreeInternalPage *>(page->GetData());
        root->Init(page_id, INVALID_PAGE_ID, internal_max_size_);
        root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());

        old_node->SetParentPageId(page_id);
        new_node->SetParentPageId(page_id);

        buffer_pool_manager_->UnpinPage(page_id, true);
        return;
    }

    // Fetch parent
    page_id_t parent_id = old_node->GetParentPageId();
    Page *parent_page = buffer_pool_manager_->FetchPage(parent_id);
    BPlusTreeInternalPage *parent = reinterpret_cast<BPlusTreeInternalPage *>(parent_page->GetData());

    if (parent->GetSize() < parent->GetMaxSize())
    {
        parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
        buffer_pool_manager_->UnpinPage(parent_id, true);
        return;
    }

    // Split parent (recursive)
    // 1. Insert into current page (assuming we have margin or temporarily overflow)
    parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());

    // 2. Split
    BPlusTreeInternalPage *new_parent = Split(parent);

    // 3. For internal node split, the first key of new node moves UP to parent
    // Typically, internal node keys separate ranges.
    // [P0 K1 P1 K2 P2] -> Split -> [P0 K1 P1] (Key K2 moves UP) [P2 ...]
    KeyType up_key = new_parent->KeyAt(0); // This is K2

    // Insert K2 into grandparent
    InsertIntoParent(parent, up_key, new_parent);

    buffer_pool_manager_->UnpinPage(parent_id, true);
    buffer_pool_manager_->UnpinPage(new_parent->GetPageId(), true);
}
