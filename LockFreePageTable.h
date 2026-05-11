#pragma once

#include <atomic>
#include <vector>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define CPU_RELAX() _mm_pause()
#else
#include <thread>
#define CPU_RELAX() std::this_thread::yield()
#endif

// Typedef definitions fallback (should be matched with Page.h)
using page_id_t = int;
using frame_id_t = int;

/**
 * LockFreePageTable is a concurrent hash table based on linear probing
 * which completely avoids mutexes by utilizing atomic Compare-And-Swap (CAS)
 * over packed 64-bit variables.
 */
class LockFreePageTable
{
private:
    struct Slot
    {
        std::atomic<uint64_t> kv; // [32 bit page_id | 32 bit frame_id]

        Slot()
        {
            kv.store(0, std::memory_order_relaxed);
        }
    };

    std::vector<Slot> table_;
    size_t capacity_;
    size_t mask_;

    static constexpr uint32_t EMPTY_KEY = 0xFFFFFFFF;
    static constexpr uint32_t TOMBSTONE_KEY = 0xFFFFFFFE;

    static size_t NextPowerOf2(size_t v)
    {
        if (v == 0)
            return 1;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }

    size_t hash(uint32_t key) const
    {
        key ^= key >> 16;
        key *= 0x85ebca6b;
        key ^= key >> 13;
        key *= 0xc2b2ae35;
        key ^= key >> 16;
        return key & mask_;
    }

public:
    explicit LockFreePageTable(size_t capacity) : capacity_(NextPowerOf2(capacity * 2)), mask_(capacity_ - 1)
    {
        table_ = std::vector<Slot>(capacity_);
        for (auto &slot : table_)
        {
            uint64_t init_val = (static_cast<uint64_t>(EMPTY_KEY) << 32) | 0;
            slot.kv.store(init_val, std::memory_order_relaxed);
        }
    }

    // Disable copy and assign
    LockFreePageTable(const LockFreePageTable &) = delete;
    LockFreePageTable &operator=(const LockFreePageTable &) = delete;

    /**
     * Retrieve the frame_id mapped to the page_id.
     * @return true if mapping found, false otherwise.
     */
    bool Find(page_id_t page_id, frame_id_t *frame_id_out)
    {
        uint32_t key = static_cast<uint32_t>(page_id);
        size_t idx = hash(key);
        size_t start_idx = idx;

        while (true)
        {
            uint64_t current_kv = table_[idx].kv.load(std::memory_order_relaxed);
            uint32_t curr_key = static_cast<uint32_t>(current_kv >> 32);

            if (curr_key == EMPTY_KEY)
            {
                return false;
            }
            else if (curr_key == key)
            {
                if (frame_id_out)
                {
                    *frame_id_out = static_cast<frame_id_t>(current_kv & 0xFFFFFFFF);
                    // 提供一次轻量的 Acquire 防止重排数据读取，代替以往每次遍历必须硬扛 Acquire 的超高开销
                    std::atomic_thread_fence(std::memory_order_acquire);
                }
                return true;
            }

            idx = (idx + 1) & mask_;
            if (idx == start_idx)
            {
                return false;
            }
        }
    }

    /**
     * Insert a page_id -> frame_id mapping into the table concurrently.
     * Reuses empty or tombstone slots to mitigate collisions.
     * @return true on success, false if table full
     */
    bool Insert(page_id_t page_id, frame_id_t frame_id)
    {
        uint32_t key = static_cast<uint32_t>(page_id);
        uint32_t value = static_cast<uint32_t>(frame_id);
        uint64_t new_kv = (static_cast<uint64_t>(key) << 32) | value;

        size_t idx = hash(key);
        size_t start_idx = idx;

        while (true)
        {
            uint64_t current_kv = table_[idx].kv.load(std::memory_order_relaxed);
            uint32_t curr_key = static_cast<uint32_t>(current_kv >> 32);

            if (curr_key == EMPTY_KEY || curr_key == TOMBSTONE_KEY)
            {
                if (table_[idx].kv.compare_exchange_weak(current_kv, new_kv, std::memory_order_release, std::memory_order_relaxed))
                {
                    return true;
                }
                CPU_RELAX();
                continue; // CAS failed, retry this slot
            }
            else if (curr_key == key)
            {
                // Key exists, update value mapped to it
                if (table_[idx].kv.compare_exchange_weak(current_kv, new_kv, std::memory_order_release, std::memory_order_relaxed))
                {
                    return true;
                }
                CPU_RELAX();
                continue;
            }

            idx = (idx + 1) & mask_;
            if (idx == start_idx)
            {
                return false; // Table is perfectly full
            }
        }
    }

    /**
     * Remove the entry mapping for the given page_id by marking it with a tombstone logic point.
     * Safe under concurrency execution loops.
     */
    bool Erase(page_id_t page_id)
    {
        uint32_t key = static_cast<uint32_t>(page_id);
        size_t idx = hash(key);
        size_t start_idx = idx;

        while (true)
        {
            uint64_t current_kv = table_[idx].kv.load(std::memory_order_relaxed);
            uint32_t curr_key = static_cast<uint32_t>(current_kv >> 32);

            if (curr_key == EMPTY_KEY)
            {
                return false; // Not found
            }
            else if (curr_key == key)
            {
                uint64_t tombstone_kv = (static_cast<uint64_t>(TOMBSTONE_KEY) << 32) | 0;
                if (table_[idx].kv.compare_exchange_weak(current_kv, tombstone_kv, std::memory_order_release, std::memory_order_relaxed))
                {
                    return true;
                }
                CPU_RELAX();
                continue; // CAS failed, retry
            }

            idx = (idx + 1) & mask_;
            if (idx == start_idx)
            {
                return false;
            }
        }
    }
};