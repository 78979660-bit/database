#pragma once

#include <cstring>
#include <iostream>

// 类型定义
using page_id_t = int;
using lsn_t = int; // Log Sequence Number (日志序列号)

// 常量定义
static constexpr int PAGE_SIZE = 4096;           // 4KB 页大小
static constexpr page_id_t INVALID_PAGE_ID = -1; // 无效页ID

/**
 * Page 类是数据库中内存管理的基本单位。
 * 它包含实际的数据（data_）和一些元数据（page_id, is_dirty, pin_count 等）。
 * BufferPoolManager 将负责在磁盘和内存之间移动这些 Page。
 */
class Page
{
    // 允许 BufferPoolManager 访问私有成员（如果以后实现了 BufferPoolManager）
    friend class BufferPoolManager;

public:
    Page() { ResetMemory(); }
    ~Page() = default;

    /** @return 实际存储数据的字符数组指针 */
    inline char *GetData() { return data_; }

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

protected:
    static_assert(sizeof(page_id_t) == 4);
    static_assert(sizeof(lsn_t) == 4);

    // 实际存储数据的 4KB 数组
    // 使用 memset 初始化为 0
    char data_[PAGE_SIZE]{};

    // 元数据
    page_id_t page_id_ = INVALID_PAGE_ID;
    int pin_count_ = 0;
    bool is_dirty_ = false;
    lsn_t lsn_ = 0; // Log Sequence Number

    /** 将数据区域清零 */
    void ResetMemory()
    {
        memset(data_, 0, PAGE_SIZE);
    }
};
