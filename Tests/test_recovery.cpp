#include "Recovery/RecoveryManager.h"
#include "DiskManager.h"
#include "BufferPoolManager.h"
#include <cassert>
#include <iostream>
#include <cstring>
#include <vector>

int main()
{
    std::string db_file = "test_recovery.db";
    std::string log_file = "test_recovery.db.log";
    remove(db_file.c_str());
    remove(log_file.c_str());

    std::cout << "Test 1: Normal Operation & Crash Simulation" << std::endl;
    // 1. Normal Operation
    {
        DiskManager dm(db_file);
        LogManager lm(&dm);
        lm.RunFlushThread();
        BufferPoolManager bpm(10, &dm, &lm);

        page_id_t page_id;
        Page *page = bpm.NewPage(&page_id);
        assert(page != nullptr);
        std::cout << "Allocated Page ID: " << page_id << std::endl;

        // Write data
        char data[] = "Hello Recovery";
        memcpy(page->GetData(), data, sizeof(data));

        // Log it
        std::string before("");
        std::string after(data, sizeof(data));
        LogRecord rec(1, INVALID_LSN, LogRecordType::INSERT, page_id, 0, before, after);

        lsn_t lsn = lm.AppendLogRecord(&rec);
        page->SetLSN(lsn);
        std::cout << "Logged Change with LSN: " << lsn << std::endl;

        // Commit
        LogRecord commit_rec(1, lsn, LogRecordType::COMMIT);
        lm.AppendLogRecord(&commit_rec);
        // Commit (Flush log)
        lm.Flush(true);
        std::cout << "Log Flushed." << std::endl;

        // Unpin checking dirty
        bpm.UnpinPage(page_id, true);

        // Simulate Crash: scope exit destroys bpm, memory lost.
        // DiskManager writes are only triggered if we flushed explicitly or evicted.
        // We do neither, so DB file should not contain "Hello Recovery".
    }

    std::cout << "Test 2: Recovery" << std::endl;
    // 2. Recovery
    {
        DiskManager dm(db_file);
        LogManager lm(&dm);
        lm.RunFlushThread();
        BufferPoolManager bpm(10, &dm, &lm);
        RecoveryManager rm(&lm, &bpm);

        // Verify page is empty initially (read from disk)
        Page *page = bpm.FetchPage(0);
        if (page)
        {
            std::cout << "Content before recovery: " << (page->GetData()[0] == 0 ? "Empty" : page->GetData()) << std::endl;
            bpm.UnpinPage(0, false);
        }

        // Run ARIES
        rm.ARIES();

        page = bpm.FetchPage(0);
        if (page)
        {
            std::cout << "Content after recovery: " << page->GetData() << std::endl;
            if (strcmp(page->GetData(), "Hello Recovery") == 0)
            {
                std::cout << "SUCCESS: Data recovered!" << std::endl;
            }
            else
            {
                std::cout << "FAILURE: Data mismatch!" << std::endl;
            }
            bpm.UnpinPage(0, false);
        }
    }

    return 0;
}
 

