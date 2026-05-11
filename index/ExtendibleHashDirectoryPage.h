#pragma once

#include "../Page.h"

namespace Database
{

    /**
     * ExtendibleHashDirectoryPage stores the directory of the Extendible Hash Index.
     *
     * Directory Layout:
     * ------------------------------------------------------------------
     * | PageId(4) | LSN(4) | GlobalDepth(4) |
     * ------------------------------------------------------------------
     * | BucketPageId[0] | BucketPageId[1] | ... | BucketPageId[2^GlobalDepth - 1] |
     * ------------------------------------------------------------------
     */
    class ExtendibleHashDirectoryPage : public Page
    {
    public:
        static constexpr uint32_t PAGE_ID_OFFSET = 0;
        static constexpr uint32_t LSN_OFFSET = 4;
        static constexpr uint32_t GLOBAL_DEPTH_OFFSET = 8;
        static constexpr uint32_t BUCKET_ARRAY_OFFSET = 12;

        void Init(page_id_t page_id)
        {
            SetPageId(page_id);
            SetLSN(0);
            SetGlobalDepth(0);
            SetBucketPageId(0, INVALID_PAGE_ID);
        }

        page_id_t GetPageId() const { return *reinterpret_cast<const page_id_t *>(GetData() + PAGE_ID_OFFSET); }
        void SetPageId(page_id_t page_id) { *reinterpret_cast<page_id_t *>(GetData() + PAGE_ID_OFFSET) = page_id; }

        void SetLSN(lsn_t lsn) { *reinterpret_cast<lsn_t *>(GetData() + LSN_OFFSET) = lsn; }

        uint32_t GetGlobalDepth() const { return *reinterpret_cast<const uint32_t *>(GetData() + GLOBAL_DEPTH_OFFSET); }
        void SetGlobalDepth(uint32_t depth) { *reinterpret_cast<uint32_t *>(GetData() + GLOBAL_DEPTH_OFFSET) = depth; }

        uint32_t GetSize() const { return 1 << GetGlobalDepth(); }

        page_id_t GetBucketPageId(uint32_t bucket_idx) const
        {
            return *reinterpret_cast<const page_id_t *>(GetData() + BUCKET_ARRAY_OFFSET + bucket_idx * sizeof(page_id_t));
        }

        void SetBucketPageId(uint32_t bucket_idx, page_id_t bucket_page_id)
        {
            *reinterpret_cast<page_id_t *>(GetData() + BUCKET_ARRAY_OFFSET + bucket_idx * sizeof(page_id_t)) = bucket_page_id;
        }

        // Double the directory
        void IncGlobalDepth()
        {
            uint32_t old_size = GetSize();
            uint32_t old_depth = GetGlobalDepth();

            SetGlobalDepth(old_depth + 1);

            // Mirror the second half to the first half
            for (uint32_t i = 0; i < old_size; ++i)
            {
                SetBucketPageId(i + old_size, GetBucketPageId(i));
            }
        }
    };

} // namespace Database