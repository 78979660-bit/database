#pragma once

#include "../Page.h"
#include <vector>

namespace Database
{

    /**
     * ExtendibleHashBucketPage Layout:
     * ------------------------------------------------------------------
     * | PageId(4) | LSN(4) | LocalDepth(4) | Count(4) | MaxSize(4) |
     * ------------------------------------------------------------------
     * | Key[0] | Val[0] | Key[1] | Val[1] | ...
     * ------------------------------------------------------------------
     */
    template <typename KeyType, typename ValueType, typename KeyComparator>
    class ExtendibleHashBucketPage : public Page
    {
    public:
        static constexpr uint32_t PAGE_ID_OFFSET = 0;
        static constexpr uint32_t LSN_OFFSET = 4;
        static constexpr uint32_t LOCAL_DEPTH_OFFSET = 8;
        static constexpr uint32_t COUNT_OFFSET = 12;
        static constexpr uint32_t MAX_SIZE_OFFSET = 16;
        static constexpr uint32_t DATA_ARRAY_OFFSET = 20;

        void Init(page_id_t page_id, uint32_t local_depth)
        {
            SetPageId(page_id);
            SetLSN(0);
            SetLocalDepth(local_depth);
            SetCount(0);
            uint32_t max_size = (PAGE_SIZE - DATA_ARRAY_OFFSET) / (sizeof(KeyType) + sizeof(ValueType));
            SetMaxSize(max_size);
        }

        page_id_t GetPageId() const { return *reinterpret_cast<const page_id_t *>(GetData() + PAGE_ID_OFFSET); }
        void SetPageId(page_id_t page_id) { *reinterpret_cast<page_id_t *>(GetData() + PAGE_ID_OFFSET) = page_id; }
        void SetLSN(lsn_t lsn) { *reinterpret_cast<lsn_t *>(GetData() + LSN_OFFSET) = lsn; }

        uint32_t GetLocalDepth() const { return *reinterpret_cast<const uint32_t *>(GetData() + LOCAL_DEPTH_OFFSET); }
        void SetLocalDepth(uint32_t depth) { *reinterpret_cast<uint32_t *>(GetData() + LOCAL_DEPTH_OFFSET) = depth; }

        uint32_t GetCount() const { return *reinterpret_cast<const uint32_t *>(GetData() + COUNT_OFFSET); }
        void SetCount(uint32_t count) { *reinterpret_cast<uint32_t *>(GetData() + COUNT_OFFSET) = count; }

        uint32_t GetMaxSize() const { return *reinterpret_cast<const uint32_t *>(GetData() + MAX_SIZE_OFFSET); }
        void SetMaxSize(uint32_t max_size) { *reinterpret_cast<uint32_t *>(GetData() + MAX_SIZE_OFFSET) = max_size; }

        bool IsFull() const { return GetCount() == GetMaxSize(); }

        KeyType KeyAt(uint32_t index) const
        {
            return *reinterpret_cast<const KeyType *>(GetData() + DATA_ARRAY_OFFSET + index * (sizeof(KeyType) + sizeof(ValueType)));
        }

        ValueType ValueAt(uint32_t index) const
        {
            return *reinterpret_cast<const ValueType *>(GetData() + DATA_ARRAY_OFFSET + index * (sizeof(KeyType) + sizeof(ValueType)) + sizeof(KeyType));
        }

        void SetKeyAt(uint32_t index, const KeyType &key)
        {
            *reinterpret_cast<KeyType *>(GetData() + DATA_ARRAY_OFFSET + index * (sizeof(KeyType) + sizeof(ValueType))) = key;
        }

        void SetValueAt(uint32_t index, const ValueType &val)
        {
            *reinterpret_cast<ValueType *>(GetData() + DATA_ARRAY_OFFSET + index * (sizeof(KeyType) + sizeof(ValueType)) + sizeof(KeyType)) = val;
        }

        bool Insert(const KeyType &key, const ValueType &val, const KeyComparator &cmp)
        {
            if (IsFull())
                return false;

            uint32_t count = GetCount();
            SetKeyAt(count, key);
            SetValueAt(count, val);
            SetCount(count + 1);
            return true;
        }

        bool Remove(const KeyType &key, const ValueType &val, const KeyComparator &cmp)
        {
            uint32_t count = GetCount();
            for (uint32_t i = 0; i < count; ++i)
            {
                if (cmp(KeyAt(i), key) == 0 && ValueAt(i) == val)
                {
                    // Swap with the last element and pop
                    SetKeyAt(i, KeyAt(count - 1));
                    SetValueAt(i, ValueAt(count - 1));
                    SetCount(count - 1);
                    return true;
                }
            }
            return false;
        }
    };

} // namespace Database