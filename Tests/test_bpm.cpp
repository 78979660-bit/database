#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <cstring>
#include <cstdio>
#include "BufferPoolManager.h"
#include "DiskManager.h"

int main()
{
    const std::string db_file = "test.db";
    std::remove(db_file.c_str()); // Ensure fresh database
    DiskManager disk_manager(db_file);
    BufferPoolManager bpm(10, &disk_manager); // Pool size 10

    std::cout << "BufferPoolManager initialized." << std::endl;

    // 1. Create a new page
    page_id_t page_id_temp;
    Page *page0 = bpm.NewPage(&page_id_temp);
    assert(page0 != nullptr);
    assert(page_id_temp == 0);
    std::cout << "Created Page 0" << std::endl;

    // 2. Write data to page 0
    std::strcpy(page0->GetData(), "Hello DB");

    // 3. Unpin page 0 (mark dirty)
    bpm.UnpinPage(page_id_temp, true);
    std::cout << "Unpinned Page 0 (Dirty)" << std::endl;

    // 4. Fetch page 0 again
    Page *page0_fetched = bpm.FetchPage(0);
    assert(page0_fetched != nullptr);
    assert(std::strcmp(page0_fetched->GetData(), "Hello DB") == 0);
    std::cout << "Fetched Page 0: " << page0_fetched->GetData() << std::endl;
    bpm.UnpinPage(0, false);

    // 5. Fill buffer pool
    std::vector<page_id_t> pages;
    for (int i = 0; i < 10; ++i)
    {
        page_id_t pid;
        Page *p = bpm.NewPage(&pid);
        pages.push_back(pid);
        std::string content = "Page " + std::to_string(pid);
        std::strcpy(p->GetData(), content.c_str());
        bpm.UnpinPage(pid, true);
    }
    // Now pool has pages 1 to 10. Page 0 might not have been evicted depending on the number of instances.
    // Explicitly flush page 0 to disk to ensure we can read it back via another BPM or DiskManager if we wanted to
    bpm.FlushPage(0);

    // 6. Fetch Page 0 from disk
    Page *page0_disk = bpm.FetchPage(0);
    assert(page0_disk != nullptr);
    assert(std::strcmp(page0_disk->GetData(), "Hello DB") == 0);
    std::cout << "Fetched Page 0 from disk: " << page0_disk->GetData() << std::endl;

    // Clean up
    remove(db_file.c_str());
    return 0;
}
