#pragma once

#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <array>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <iostream>

#include "DiskManager.h"
#include "Page.h"
#include "Replacer.h"
#include "LockFreePageTable.h"
#include "LockFreeRingBuffer.h"
#include "./Recovery/LogManager.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

class BufferPoolManager
{
private:
    static constexpr size_t NUM_INSTANCES = 32;

    struct alignas(64) BufferPoolInstance
    {
        BufferPoolInstance(size_t instance_pool_size)
            : pool_size(instance_pool_size),
              page_table(instance_pool_size),
              free_list(instance_pool_size),
              replacer(new ClockSweepReplacer(instance_pool_size)),
              pages(nullptr),
              memory_slab(nullptr),
              actual_alloc_size(0)
        {
            // Allocate both Page metadata array and data payload on a single huge memory slab
            // This maximizes data locality when Page methods lookup data_ pointers (avoids separate TLB miss)
            size_t metadata_size = instance_pool_size * sizeof(Page);
            size_t data_offset = (metadata_size + 4095) & ~4095; // Ensure 4KB alignment for data_slab
            size_t data_size = instance_pool_size * PAGE_SIZE;
            size_t alloc_size = data_offset + data_size;

#ifdef _WIN32
            memory_slab = static_cast<char *>(VirtualAlloc(NULL, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            actual_alloc_size = alloc_size;
#else
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
            // Try to allocate using huge pages
            memory_slab = static_cast<char *>(mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0));
            // Fallback if OS doesn't have sufficient Huge Pages configured
            if (memory_slab == MAP_FAILED || memory_slab == reinterpret_cast<char *>(-1))
            {
                memory_slab = static_cast<char *>(mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#ifdef MADV_HUGEPAGE
                madvise(memory_slab, alloc_size, MADV_HUGEPAGE);
#endif
            }
            actual_alloc_size = alloc_size;
#endif
            if (!memory_slab || memory_slab == reinterpret_cast<char *>(-1))
            {
                throw std::runtime_error("Failed to allocate huge memory slab for Buffer Pool Instance");
            }

            // Map the Page objects and payload directly onto the contiguous memory_slab
            pages = reinterpret_cast<Page *>(memory_slab);
            char *data_slab = memory_slab + data_offset;

            for (size_t i = 0; i < instance_pool_size; ++i)
            {
                // In-place initialization
                new (&pages[i]) Page();
                pages[i].data_ = data_slab + i * PAGE_SIZE;
                pages[i].ResetMemory();
                free_list.Push(static_cast<int>(i));
            }
        }

        ~BufferPoolInstance()
        {
            if (pages)
            {
                for (size_t i = 0; i < pool_size; ++i)
                {
                    pages[i].~Page();
                }
            }

#ifdef _WIN32
            VirtualFree(memory_slab, 0, MEM_RELEASE);
#else
            if (memory_slab && memory_slab != reinterpret_cast<char *>(-1))
                munmap(memory_slab, actual_alloc_size);
#endif
            delete replacer;
        }

        size_t pool_size;
        LockFreePageTable page_table;
        LockFreeRingBuffer<frame_id_t> free_list;
        Replacer *replacer;
        Page *pages;
        char *memory_slab;
        size_t actual_alloc_size;
    };

public:
    BufferPoolManager(size_t pool_size, DiskManager *disk_manager, LogManager *log_manager = nullptr);
    ~BufferPoolManager();

    void StartBackgroundFlusher();
    void StopBackgroundFlusher();
    Page *FetchPage(page_id_t page_id);
    Page *NewPage(page_id_t *page_id);
    void PrefetchRange(page_id_t start_page_id, int count = 16);
    bool UnpinPage(page_id_t page_id, bool is_dirty);
    bool FlushPage(page_id_t page_id);
    void FlushAllPages();
    bool DeletePage(page_id_t page_id);

private:
    size_t GetInstanceIndex(page_id_t page_id) const
    {
        return std::hash<page_id_t>{}(page_id) & (NUM_INSTANCES - 1);
    }
    bool FindFreeFrame(BufferPoolInstance &instance, frame_id_t *frame_id);

    size_t pool_size_;
    DiskManager *disk_manager_;
    LogManager *log_manager_;
    std::vector<BufferPoolInstance *> instances_;

    std::atomic<bool> enable_background_flusher_{false};
    std::thread background_flusher_thread_;
    std::mutex bg_flusher_mutex_;
    std::condition_variable bg_flusher_cv_;
    void BackgroundFlushWorker();
};
