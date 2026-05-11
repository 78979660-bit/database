#pragma once

#include "LogRecord.h"
#include "../../Database/DiskManager.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

class LogManager
{
public:
    LogManager(DiskManager *disk_manager) : disk_manager_(disk_manager) {}
    ~LogManager();

    void RunFlushThread();
    void StopFlushThread();

    lsn_t AppendLogRecord(LogRecord *log_record);
    void Flush(bool force = false);

    // Group Commit triggers: ������ô˷��������ȴ����� LSN ����
    void WaitForFlush(lsn_t target_lsn);

    inline DiskManager *GetDiskManager() { return disk_manager_; }
    inline lsn_t GetPersistentLSN() const { return persistent_lsn_.load(); }

private:
    static constexpr int LOG_BUFFER_SIZE = 32 * 1024;

    std::atomic<uint64_t> write_head_{0}; // Points to next available offset in log_buffer_
    std::atomic<uint64_t> flush_tail_{0}; // Points to next offset to flush to disk

    char log_buffer_[LOG_BUFFER_SIZE];

    std::atomic<lsn_t> next_lsn_{0};
    std::atomic<lsn_t> persistent_lsn_{-1}; // 已持久化的 LSN

    std::mutex latch_;
    std::condition_variable cv_;        // 触发等待后台刷新线程
    std::condition_variable append_cv_; // 触发等待 Group Commit 完成

    std::thread *flush_thread_{nullptr};
    std::atomic<bool> enable_logging_{false};
    std::atomic<bool> force_flush_{false};

    DiskManager *disk_manager_;
};
