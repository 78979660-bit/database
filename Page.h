#pragma once

#include <cstring>
#include <iostream>
#include <shared_mutex>
#include <atomic>

// 类型定义
using page_id_t = int;
using lsn_t = int; // Log Sequence Number (日志序列号)

#ifdef _WIN32
#include <malloc.h>
#else
#include <stdlib.h>
#include <sys/mman.h> // 提供 madvise/mmap 指令
#endif

// 常量定义
static constexpr int PAGE_SIZE = 4096;           // 4KB 页大小（如果是巨大的数据可以开 2MB 大页）
static constexpr page_id_t INVALID_PAGE_ID = -1; // 无效页ID

/**
 * Page 类是数据库中内存管理的基本单位。
 * 它包含实际的数据（data_）和一些元数据（page_id, is_dirty, pin_count 等）。
 * BufferPoolManager 将负责在磁盘和内存之间移动这些 Page。
 */
class alignas(64) Page
{
    // 允许 BufferPoolManager 访问私有成员（如果以后实现了 BufferPoolManager）
    friend class BufferPoolManager;

public:
    Page()
    {
        // 实际的内存分配 (data_) 将由缓冲池统一使用 OS mmap/VirtualAlloc 做大块 Slab 分配
        // 这里只是一个轻量级的元数据初始化
        data_ = nullptr;
    }
    ~Page()
    {
        // 不再由 Page 自行销毁内存
    }

    /** @return 实际存储数据的字符数组指针 */
    inline char *GetData() { return data_; }
    inline const char *GetData() const { return data_; }

    /** @return 该页的页ID */
    inline page_id_t GetPageId() { return page_id_; }

    /** @return 该页的引用计数 (被多少个线程/进程使用) */
    inline int GetPinCount() { return pin_count_; }

    /** @return 如果该页被修改过（脏页），返回 true */
    inline bool IsDirty() { return is_dirty_; }

    /** @return 获取日志序列号 (用于预写日志 WAL) */
    inline lsn_t GetLSN() { return lsn_; }

    /** 设置日志序列号 */
    inline void SetLSN(lsn_t lsn) { lsn_ = lsn; }

    // [拆毁防线: 废弃传统悲观锁] 完全拥抱 Bw-Tree 及乐观/无锁机制
    // 直接掏空原有的共享读写锁 (std::shared_mutex)，杜绝内核态线程挂起
    inline void WLatch()
    {
        version_.fetch_add(1, std::memory_order_release);
    }
    inline void WUnlatch()
    {
        version_.fetch_add(1, std::memory_order_release);
    }
    inline void RLatch() { /* 纯内存引擎无锁查询，无阻塞 */ }
    inline void RUnlatch() { /* 无需释放 */ }

    inline uint32_t GetVersion() const
    {
        return version_.load(std::memory_order_acquire);
    }
    inline bool CheckVersion(uint32_t version) const
    {
        return version == version_.load(std::memory_order_acquire) && (version % 2 == 0);
    }

protected:
    static_assert(sizeof(page_id_t) == 4);
    static_assert(sizeof(lsn_t) == 4);

    // 实际存储数据的 4KB 数组
    // 动态分配，确保满足 Direct I/O 对齐要求
    char *data_ = nullptr;

    // [拆毁防线: 完全内存化的 Bw-Tree 架构] Deta Record 链表头指针
    // 所有更新 (Update/Insert) 将采用无锁 CAS 挂载 Delta 节点
    struct DeltaHeader
    {
        void *record_ptr = nullptr;
    };
    std::atomic<DeltaHeader *> delta_head_{nullptr};

    // 元数据
    page_id_t page_id_ = INVALID_PAGE_ID;
    std::atomic<int> pin_count_{0};
    std::atomic<bool> is_dirty_{false};
    lsn_t lsn_ = 0; // Log Sequence Number
    // std::shared_mutex rwlatch_; // 被移除: 纯无锁结构
    std::atomic<uint32_t> version_{0};

    // BwTree CAS 安装 Delta 的通用接口
    inline bool InstallDelta(DeltaHeader *expected, DeltaHeader *new_delta)
    {
        return delta_head_.compare_exchange_weak(expected, new_delta, std::memory_order_release, std::memory_order_relaxed);
    }

    /** 将数据区域清零 */
    void ResetMemory()
    {
        if (data_)
        {
            memset(data_, 0, PAGE_SIZE);
        }
    }
};
