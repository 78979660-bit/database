#pragma once

#include <string>
#include <vector>
#include "../BufferPoolManager.h"
#include "ExtendibleHashDirectoryPage.h"
#include "ExtendibleHashBucketPage.h"
#include <functional> // struct std::hash

namespace Database
{

    template <typename KeyType, typename ValueType, typename KeyComparator>
    class ExtendibleHashTable
    {
    public:
        ExtendibleHashTable(const std::string &name, BufferPoolManager *bpm, const KeyComparator &cmp, const std::hash<KeyType> &hash_fn)
            : name_(name), bpm_(bpm), cmp_(cmp), hash_fn_(hash_fn)
        {
            // 1. Initialize Directory
            Page *dir_page = bpm_->NewPage(&directory_page_id_);
            auto *dir = reinterpret_cast<ExtendibleHashDirectoryPage *>(dir_page);
            dir->Init(directory_page_id_);

            // 2. Initialize first bucket
            page_id_t bucket_page_id;
            Page *bucket_page = bpm_->NewPage(&bucket_page_id);
            auto *bucket = reinterpret_cast<ExtendibleHashBucketPage<KeyType, ValueType, KeyComparator> *>(bucket_page);
            bucket->Init(bucket_page_id, 0); // initial local depth 0

            dir->SetBucketPageId(0, bucket_page_id);

            bpm_->UnpinPage(bucket_page_id, true);
            bpm_->UnpinPage(directory_page_id_, true);
        }

        // Hash logic
        uint32_t GetHash(KeyType key)
        {
            return static_cast<uint32_t>(hash_fn_(key));
        }

        uint32_t GetDirectoryIndex(uint32_t hash, uint32_t global_depth)
        {
            return hash & ((1 << global_depth) - 1);
        }

        bool Insert(const KeyType &key, const ValueType &value)
        {
            uint32_t hash = GetHash(key);
            return InsertImpl(key, value, hash);
        }

        bool GetValue(const KeyType &key, std::vector<ValueType> *result)
        {
            Page *dir_page = bpm_->FetchPage(directory_page_id_);
            auto *dir = reinterpret_cast<ExtendibleHashDirectoryPage *>(dir_page);

            uint32_t hash = GetHash(key);
            uint32_t bucket_idx = GetDirectoryIndex(hash, dir->GetGlobalDepth());
            page_id_t bucket_page_id = dir->GetBucketPageId(bucket_idx);

            Page *bucket_page = bpm_->FetchPage(bucket_page_id);
            auto *bucket = reinterpret_cast<ExtendibleHashBucketPage<KeyType, ValueType, KeyComparator> *>(bucket_page);

            bool found = false;
            uint32_t count = bucket->GetCount();
            for (uint32_t i = 0; i < count; ++i)
            {
                if (cmp_(bucket->KeyAt(i), key) == 0)
                {
                    result->push_back(bucket->ValueAt(i));
                    found = true;
                }
            }

            bpm_->UnpinPage(bucket_page_id, false);
            bpm_->UnpinPage(directory_page_id_, false);
            return found;
        }

    private:
        bool InsertImpl(const KeyType &key, const ValueType &value, uint32_t hash)
        {
            Page *dir_page = bpm_->FetchPage(directory_page_id_);
            auto *dir = reinterpret_cast<ExtendibleHashDirectoryPage *>(dir_page);

            uint32_t bucket_idx = GetDirectoryIndex(hash, dir->GetGlobalDepth());
            page_id_t bucket_page_id = dir->GetBucketPageId(bucket_idx);

            Page *bucket_page = bpm_->FetchPage(bucket_page_id);
            auto *bucket = reinterpret_cast<ExtendibleHashBucketPage<KeyType, ValueType, KeyComparator> *>(bucket_page);

            // Try direct insert
            if (!bucket->IsFull())
            {
                bucket->Insert(key, value, cmp_);
                bpm_->UnpinPage(bucket_page_id, true);
                bpm_->UnpinPage(directory_page_id_, false);
                return true;
            }

            // Split routine
            uint32_t local_depth = bucket->GetLocalDepth();

            // 1. Directory Expansion if needed
            if (local_depth == dir->GetGlobalDepth())
            {
                dir->IncGlobalDepth();
            }

            // 2. Allocate new Bucket
            page_id_t new_bucket_page_id;
            Page *new_bucket_page = bpm_->NewPage(&new_bucket_page_id);
            auto *new_bucket = reinterpret_cast<ExtendibleHashBucketPage<KeyType, ValueType, KeyComparator> *>(new_bucket_page);
            new_bucket->Init(new_bucket_page_id, local_depth + 1);

            // Update local depths
            bucket->SetLocalDepth(local_depth + 1);

            // 3. Redistribute entries
            uint32_t count = bucket->GetCount();
            std::vector<std::pair<KeyType, ValueType>> temp_records;
            for (uint32_t i = 0; i < count; ++i)
            {
                temp_records.push_back({bucket->KeyAt(i), bucket->ValueAt(i)});
            }
            bucket->SetCount(0); // clear

            // Reinsert into correct buckets
            for (const auto &record : temp_records)
            {
                uint32_t item_hash = GetHash(record.first);
                // Check the (local_depth) bit to decide the target bucket
                if (item_hash & (1 << local_depth))
                {
                    new_bucket->Insert(record.first, record.second, cmp_);
                }
                else
                {
                    bucket->Insert(record.first, record.second, cmp_);
                }
            }

            // 4. Update directory pointers
            // New bucket identifier bit pattern: suffix with the new bit set to 1
            uint32_t mask = (1 << local_depth) - 1;
            uint32_t old_suffix = bucket_idx & mask;
            uint32_t new_target = old_suffix | (1 << local_depth);

            // Loop through all items in the directory hitting this new suffix boundary
            uint32_t dir_size = dir->GetSize();
            uint32_t step = 1 << (local_depth + 1);
            for (uint32_t i = new_target; i < dir_size; i += step)
            {
                dir->SetBucketPageId(i, new_bucket_page_id);
            }

            // Release resources cleanly and recursively try inserting again to appropriate bucket
            bpm_->UnpinPage(new_bucket_page_id, true);
            bpm_->UnpinPage(bucket_page_id, true);
            bpm_->UnpinPage(directory_page_id_, true);

            return InsertImpl(key, value, hash);
        }

        std::string name_;
        page_id_t directory_page_id_;
        BufferPoolManager *bpm_;
        KeyComparator cmp_;
        std::hash<KeyType> hash_fn_;
    };

} // namespace Database