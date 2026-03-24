#include "BufferPoolManager.h"

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager)
{
    pages_ = new Page[pool_size_];
    replacer_ = new LRUReplacer(pool_size_);
    for (size_t i = 0; i < pool_size_; ++i)
    {
        free_list_.push_back(static_cast<int>(i));
    }
}

BufferPoolManager::~BufferPoolManager()
{
    delete[] pages_;
    delete replacer_;
}

Page *BufferPoolManager::FetchPage(page_id_t page_id)
{
    std::scoped_lock lock(latch_);
    // 1. Search the page table for the requested page (P).
    if (page_table_.find(page_id) != page_table_.end())
    {
        frame_id_t frame_id = page_table_[page_id];
        pages_[frame_id].pin_count_++;
        replacer_->Pin(frame_id);
        return &pages_[frame_id];
    }
    // 2. If P is not in the buffer pool, find a victim page.
    frame_id_t frame_id = -1;
    if (!FindFreeFrame(&frame_id))
    {
        return nullptr;
    }
    // 3. Update P's metadata, zero out memory, and add P to the page table.
    Page *page = &pages_[frame_id];

    // If we are reusing a frame, we need to handle the old page
    if (page->IsDirty())
    {
        disk_manager_->WritePage(page->GetPageId(), page->GetData());
    }
    page_table_.erase(page->GetPageId());

    // Read new page from disk
    page->page_id_ = page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = false;
    disk_manager_->ReadPage(page_id, page->GetData());

    page_table_[page_id] = frame_id;
    replacer_->Pin(frame_id);

    return page;
}

Page *BufferPoolManager::NewPage(page_id_t *page_id)
{
    std::scoped_lock lock(latch_);
    frame_id_t frame_id = -1;
    if (!FindFreeFrame(&frame_id))
    {
        return nullptr;
    }
    *page_id = disk_manager_->AllocatePage();
    Page *page = &pages_[frame_id];

    if (page->IsDirty())
    {
        disk_manager_->WritePage(page->GetPageId(), page->GetData());
    }
    page_table_.erase(page->GetPageId());

    page->page_id_ = *page_id;
    page->pin_count_ = 1;
    page->is_dirty_ = true; // New page is considered dirty so it gets written eventually? Or maybe not.
                            // Actually, if we just allocated it, it's empty. If we don't write anything,
                            // we don't need to write to disk. But usually NewPage is called to write something.
                            // Let's set it to false and let the caller mark it dirty when they write.
                            // Wait, if we evict it immediately without writing, we lose the fact that it exists?
                            // DiskManager::AllocatePage just increments a counter. It doesn't write zeros to disk.
                            // If we allocate page X, and evict it without writing, checking usage file size might be enough?
                            // But for safety, let's treat it as dirty if we want to ensure space is reserved?
                            // Actually, let's stick to standard behavior: caller should Unpin(true) if modified.
    page->is_dirty_ = false;
    page->ResetMemory();

    page_table_[*page_id] = frame_id;
    replacer_->Pin(frame_id);

    return page;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty)
{
    std::scoped_lock lock(latch_);
    if (page_table_.find(page_id) == page_table_.end())
    {
        return false;
    }
    frame_id_t frame_id = page_table_[page_id];
    Page *page = &pages_[frame_id];
    if (page->pin_count_ <= 0)
    {
        return false;
    }
    page->pin_count_--;
    if (is_dirty)
    {
        page->is_dirty_ = true;
    }
    if (page->pin_count_ == 0)
    {
        replacer_->Unpin(frame_id);
    }
    return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id)
{
    std::scoped_lock lock(latch_);
    if (page_table_.find(page_id) == page_table_.end())
    {
        return false;
    }
    frame_id_t frame_id = page_table_[page_id];
    Page *page = &pages_[frame_id];
    disk_manager_->WritePage(page_id, page->GetData());
    page->is_dirty_ = false;
    return true;
}

void BufferPoolManager::FlushAllPages()
{
    std::scoped_lock lock(latch_);
    for (auto &pair : page_table_)
    {
        page_id_t page_id = pair.first;
        frame_id_t frame_id = pair.second;
        Page *page = &pages_[frame_id];
        disk_manager_->WritePage(page_id, page->GetData());
        page->is_dirty_ = false;
    }
}

bool BufferPoolManager::DeletePage(page_id_t page_id)
{
    std::scoped_lock lock(latch_);
    if (page_table_.find(page_id) == page_table_.end())
    {
        return true;
    }
    frame_id_t frame_id = page_table_[page_id];
    Page *page = &pages_[frame_id];
    if (page->pin_count_ > 0)
    {
        return false;
    }

    disk_manager_->DeallocatePage(page_id);
    page_table_.erase(page_id);

    // Reset page metadata
    page->page_id_ = INVALID_PAGE_ID;
    page->is_dirty_ = false;
    page->pin_count_ = 0;

    free_list_.push_back(frame_id);
    return true;
}

bool BufferPoolManager::FindFreeFrame(frame_id_t *frame_id)
{
    // 1. Check free list
    if (!free_list_.empty())
    {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    // 2. Check replacer
    if (replacer_->Victim(frame_id))
    {
        return true;
    }
    return false;
}
