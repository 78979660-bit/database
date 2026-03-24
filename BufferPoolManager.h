#pragma once

#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "DiskManager.h"
#include "Page.h"
#include "Replacer.h"

/**
 * BufferPoolManager manages the buffer pool.
 * The buffer pool is a fixed-size array of Page objects.
 * The BufferPoolManager uses the Replacer to keep track of unpinned pages.
 */
class BufferPoolManager
{
public:
    BufferPoolManager(size_t pool_size, DiskManager *disk_manager);

    ~BufferPoolManager();

    /**
     * Fetch the requested page with the given page_id.
     * @param page_id The ID of the page to fetch.
     * @return A pointer to the page, or nullptr if there are no free frames.
     */
    Page *FetchPage(page_id_t page_id);

    /**
     * Create a new page in the buffer pool.
     * @param[out] page_id The ID of the newly created page.
     * @return A pointer to the new page, or nullptr if there are no free frames.
     */
    Page *NewPage(page_id_t *page_id);

    /**
     * Unpin the target page from the buffer pool.
     * @param page_id The ID of the page to unpin.
     * @param is_dirty True if the page was modified.
     * @return True if the page was successfully unpinned, false if the page was not in the buffer pool.
     */
    bool UnpinPage(page_id_t page_id, bool is_dirty);

    /**
     * Flush the target page to disk.
     * @param page_id The ID of the page to flush.
     * @return True if the page was successfully flushed, false if the page was not in the buffer pool.
     */
    bool FlushPage(page_id_t page_id);

    /**
     * Flush all pages to disk.
     */
    void FlushAllPages();

    /**
     * Delete a page from the buffer pool.
     * @param page_id The ID of the page to delete.
     * @return True if the page was successfully deleted, false if the page was not in the buffer pool.
     */
    bool DeletePage(page_id_t page_id);

private:
    /**
     * Helper function to find a free frame in the buffer pool.
     * @param[out] frame_id The ID of the free frame.
     * @return True if a free frame was found, false otherwise.
     */
    bool FindFreeFrame(frame_id_t *frame_id);

    size_t pool_size_;
    Page *pages_;
    DiskManager *disk_manager_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::list<frame_id_t> free_list_;
    Replacer *replacer_;
    std::mutex latch_;
};
