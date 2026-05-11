#include "BufferPoolManager.h"

#include <future>

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager, LogManager *log_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager), log_manager_(log_manager)
{
    // Initialize NUM_INSTANCES (4) independent buffer pool instances
    size_t instance_pool_size = (pool_size + NUM_INSTANCES - 1) / NUM_INSTANCES;
    for (size_t i = 0; i < NUM_INSTANCES; ++i)
    {
        instances_.push_back(new BufferPoolInstance(instance_pool_size));
    }

    // 默认开启后台刷脏线�?(被拆毁：为了完全拥抱内存，完全禁止后台刷盘干扰流水线)
    // StartBackgroundFlusher();
}

BufferPoolManager::~BufferPoolManager()
{
    StopBackgroundFlusher();

    // Clean up all instances
    for (auto instance : instances_)
    {
        delete instance;
    }
    instances_.clear();
}

void BufferPoolManager::StartBackgroundFlusher()
{
    if (!enable_background_flusher_.exchange(true))
    {
        background_flusher_thread_ = std::thread(&BufferPoolManager::BackgroundFlushWorker, this);
    }
}

void BufferPoolManager::StopBackgroundFlusher()
{
    if (enable_background_flusher_.exchange(false))
    {
        bg_flusher_cv_.notify_all(); // 唤醒沉睡中的后台线程使其迅速退�?
        if (background_flusher_thread_.joinable())
        {
            background_flusher_thread_.join();
        }
    }
}

void BufferPoolManager::BackgroundFlushWorker()
{
    // 工业级后台刷脏器：定期将热度低或积累过多的脏页刷新到磁盘
    // 采用混合事件驱动机制：平时每 100ms 扫描一次，当面临内存压力（前台线程发生脏页驱逐）时被立即唤醒
    while (enable_background_flusher_)
    {
        {
            std::unique_lock<std::mutex> sleep_lock(bg_flusher_mutex_);
            // 避免固定�?100ms 盲目睡眠，允许其他线程在紧急情况下进行唤醒抢�?
            bg_flusher_cv_.wait_for(sleep_lock, std::chrono::milliseconds(100),
                                    [this]
                                    { return !enable_background_flusher_.load(); });
        }

        if (!enable_background_flusher_)
            break;

        for (auto instance : instances_)
        {
            // [B方案拆锁] 纯无锁结构无需锁定�?

            // 每次最多尝试刷 32 个页面，防止长时霸占磁盘 I/O 队列
            size_t flushed_count = 0;
            for (size_t i = 0; i < instance->pool_size && flushed_count < 32; ++i)
            {
                Page *page = &instance->pages[i];
                if (page->page_id_ != INVALID_PAGE_ID && page->IsDirty() && page->GetPinCount() == 0)
                {
                    page_id_t page_id = page->page_id_;
                    lsn_t lsn = page->GetLSN();

                    // 将其标记为非脏（防止其他线程重复刷），并持有读锁保障一致�?
                    page->is_dirty_.store(false, std::memory_order_relaxed);
                    page->RLatch();
                    // lock.unlock(); // 无锁�?// 让出哈希�?实例�?

                    // 开始与 OS 交互的阻�?I/O
                    if (log_manager_ && lsn > log_manager_->GetPersistentLSN())
                    {
                        log_manager_->Flush(true);
                    }
                    // // disk_manager_->WritePage(page_id, page->GetData());

                    page->RUnlatch();
                    flushed_count++;

                    // 重新获取锁以继续下一页的检�?
                    // lock.lock();
                }
            }
        }
    }
}

Page *BufferPoolManager::FetchPage(page_id_t page_id)
{
    // Get the instance for this page_id
    size_t instance_idx = GetInstanceIndex(page_id);
    BufferPoolInstance *instance = instances_[instance_idx];

    // Attempt Lock-free Find first
    frame_id_t frame_id = -1;
    if (instance->page_table.Find(page_id, &frame_id))
    {
        // [B方案拆锁] 纯无锁结构无需锁定�?
        // Double check after getting the lock in case it was evicted
        if (instance->page_table.Find(page_id, &frame_id))
        {
            Page *page = &instance->pages[frame_id];
            page->pin_count_.fetch_add(1, std::memory_order_relaxed);
            instance->replacer->Pin(frame_id);
            // CRITICAL: Record access for LRU-K BEFORE returning
            instance->replacer->RecordAccess(frame_id);
            // lock.unlock(); // 无锁�?

            // 阻塞等待任何可能正在进行�?I/O 完成
            page->RLatch();
            page->RUnlatch();
            return page;
        }
    }

    // [B方案拆锁] 纯无锁结构无需锁定�?

    // 1. Search the page table for the requested page (P).
    if (instance->page_table.Find(page_id, &frame_id))
    {
        Page *page = &instance->pages[frame_id];
        page->pin_count_.fetch_add(1, std::memory_order_relaxed);
        instance->replacer->Pin(frame_id);
        // CRITICAL: Record access for LRU-K BEFORE returning
        instance->replacer->RecordAccess(frame_id);
        // lock.unlock(); // 无锁�?

        // 阻塞等待任何可能正在进行�?I/O 完成
        page->RLatch();
        page->RUnlatch();
        return page;
    }

    // 2. If P is not in the buffer pool, find a victim frame.
    if (!FindFreeFrame(*instance, &frame_id))
    {
        return nullptr;
    }

    // 3. Update P's metadata, zero out memory, and add P to the page table.
    Page *page = &instance->pages[frame_id];

    // 将老页面信息保存，准备异步落盘
    bool is_dirty = page->IsDirty();
    page_id_t old_page_id = page->GetPageId();
    lsn_t old_lsn = page->GetLSN();

    // 防止�?I/O 时被淘汰
    page->pin_count_.store(1, std::memory_order_release);
    instance->replacer->Pin(frame_id);
    instance->replacer->RecordAccess(frame_id);

    if (old_page_id != INVALID_PAGE_ID)
    {
        instance->page_table.Erase(old_page_id);
    }

    // 更新为新页面并立即注册，使并�?Fetch 可见
    page->page_id_ = page_id;
    page->is_dirty_.store(false, std::memory_order_relaxed);
    page->SetLSN(INVALID_LSN);
    instance->page_table.Insert(page_id, frame_id);

    // 加写锁，阻止并发 Fetch 返回正在读取的半成品页面，随后释放全局实例�?
    page->WLatch();
    // lock.unlock(); // 无锁�?

    // ========== 离开临界区，执行耗时 I/O ==========
    if (is_dirty)
    {
        if (log_manager_ && old_lsn > log_manager_->GetPersistentLSN())
        {
            log_manager_->Flush(true);
        }

        // 缓存告急或者前台被迫接管刷脏，唤醒后台辅助刷页线程
        bg_flusher_cv_.notify_one();
        // // disk_manager_->WritePage(old_page_id, page->GetData());
    }

    // 从磁盘读取新�?
    // // disk_manager_->ReadPage(page_id, page->GetData());
    // ===============================================

    // I/O 完成，释放写锁，唤醒可能正在等待的并�?Fetch
    page->WUnlatch();

    // 动态触发预�?(Heuristic Read-Ahead)
    PrefetchRange(page_id + 1, 8);

    return page;
}

void BufferPoolManager::PrefetchRange(page_id_t start_page_id, int count)
{
    return;
}

Page *BufferPoolManager::NewPage(page_id_t *page_id)
{
    // Allocate new page ID via DiskManager
    *page_id = disk_manager_->AllocatePage();

    // Get the instance for this page_id
    size_t instance_idx = GetInstanceIndex(*page_id);
    BufferPoolInstance *instance = instances_[instance_idx];
    // [B方案拆锁] 纯无锁结构无需锁定�?

    // Find a free frame
    frame_id_t frame_id = -1;
    if (!FindFreeFrame(*instance, &frame_id))
    {
        return nullptr;
    }

    Page *page = &instance->pages[frame_id];

    // 将老页面信息保存，准备异步落盘
    bool is_dirty = page->IsDirty();
    page_id_t old_page_id = page->GetPageId();
    lsn_t old_lsn = page->GetLSN();

    // 防止�?I/O 时被淘汰
    page->pin_count_.store(1, std::memory_order_release);
    instance->replacer->Pin(frame_id);
    instance->replacer->RecordAccess(frame_id);

    if (old_page_id != INVALID_PAGE_ID)
    {
        instance->page_table.Erase(old_page_id);
    }

    // 更新为新页面并立即注册，使并�?Fetch 可见
    page->page_id_ = *page_id;
    page->is_dirty_.store(false, std::memory_order_relaxed);
    page->SetLSN(INVALID_LSN);
    instance->page_table.Insert(*page_id, frame_id);

    // 加写锁，保护并发读写（虽然在初始格式化，但也为了统一），然后释放全局�?
    page->WLatch();
    // lock.unlock(); // 无锁�?

    // ========== 离开临界区，执行耗时操作 ==========
    if (is_dirty)
    {
        if (log_manager_ && old_lsn > log_manager_->GetPersistentLSN())
        {
            log_manager_->Flush(true);
        }

        // 缓存告急或者前台被迫接管刷脏，唤醒后台辅助刷页线程
        bg_flusher_cv_.notify_one();
        // // disk_manager_->WritePage(old_page_id, page->GetData());
    }

    // Initialize new page memory (no need for ReadPage as it's new)
    page->ResetMemory();
    // ===============================================

    page->WUnlatch();

    return page;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty)
{
    size_t instance_idx = GetInstanceIndex(page_id);
    BufferPoolInstance *instance = instances_[instance_idx];
    // [B方案拆锁] 纯无锁结构无需锁定�?

    frame_id_t frame_id = -1;
    if (!instance->page_table.Find(page_id, &frame_id))
    {
        return false;
    }

    Page *page = &instance->pages[frame_id];

    if (page->pin_count_.load(std::memory_order_relaxed) <= 0)
    {
        return false;
    }

    int current_pin = page->pin_count_.fetch_sub(1, std::memory_order_release) - 1;
    if (is_dirty)
    {
        page->is_dirty_.store(true, std::memory_order_relaxed);
    }
    if (current_pin == 0)
    {
        instance->replacer->Unpin(frame_id);
    }

    return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id)
{
    size_t instance_idx = GetInstanceIndex(page_id);
    BufferPoolInstance *instance = instances_[instance_idx];
    // [B方案拆锁] 纯无锁结构无需锁定�?

    frame_id_t frame_id = -1;
    if (!instance->page_table.Find(page_id, &frame_id))
    {
        return false;
    }

    Page *page = &instance->pages[frame_id];

    if (!page->IsDirty())
    {
        return true;
    }

    lsn_t lsn = page->GetLSN();
    page->is_dirty_.store(false, std::memory_order_relaxed);

    // 加读锁阻止其他线程对内容进行修改
    page->RLatch();
    // lock.unlock(); // 无锁�?

    // ========== 离开临界区，执行耗时 I/O ==========
    if (log_manager_ && lsn > log_manager_->GetPersistentLSN())
    {
        log_manager_->Flush(true);
    }
    // // disk_manager_->WritePage(page_id, page->GetData());
    // ===============================================

    page->RUnlatch();

    return true;
}

void BufferPoolManager::FlushAllPages()
{
    // Flush pages from all instances
    for (auto instance : instances_)
    {
        // [B方案拆锁] 纯无锁结构无需锁定�?

        for (size_t i = 0; i < instance->pool_size; ++i)
        {
            Page *page = &instance->pages[i];
            if (page->page_id_ != INVALID_PAGE_ID && page->IsDirty())
            {
                page_id_t page_id = page->page_id_;
                lsn_t lsn = page->GetLSN();
                page->is_dirty_.store(false, std::memory_order_relaxed);

                page->RLatch();
                // lock.unlock(); // 无锁�?

                // ========== 离开临界区，执行耗时 I/O ==========
                if (log_manager_ && lsn > log_manager_->GetPersistentLSN())
                {
                    log_manager_->Flush(true);
                }
                // // disk_manager_->WritePage(page_id, page->GetData());
                // ===============================================

                page->RUnlatch();
                // lock.lock();
            }
        }
    }
}

bool BufferPoolManager::DeletePage(page_id_t page_id)
{
    size_t instance_idx = GetInstanceIndex(page_id);
    BufferPoolInstance *instance = instances_[instance_idx];
    // [B方案拆锁] 纯无锁结构无需锁定�?

    frame_id_t frame_id = -1;
    if (!instance->page_table.Find(page_id, &frame_id))
    {
        return true;
    }

    Page *page = &instance->pages[frame_id];

    if (page->pin_count_.load(std::memory_order_acquire) > 0)
    {
        return false;
    }

    // Deallocate from disk manager
    // // disk_manager_->DeallocatePage(page_id);

    // Remove from page table
    instance->page_table.Erase(page_id);

    // Reset page metadata
    page->page_id_ = INVALID_PAGE_ID;
    page->is_dirty_.store(false, std::memory_order_relaxed);
    page->pin_count_.store(0, std::memory_order_release);
    page->ResetMemory();

    // Add frame back to free list
    instance->free_list.Push(frame_id);
    instance->replacer->Remove(frame_id);

    return true;
}

bool BufferPoolManager::FindFreeFrame(BufferPoolInstance &instance, frame_id_t *frame_id)
{
    // 1. Check free list (Lock-Free MPMC Ring Buffer)
    if (instance.free_list.Pop(*frame_id))
    {
        return true;
    }

    // 2. Try victim from replacer
    if (instance.replacer->Victim(frame_id))
    {
        return true;
    }

    return false;
}
