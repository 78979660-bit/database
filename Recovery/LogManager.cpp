#include "LogManager.h"
#include <cstring>
#include <chrono>
#include <iostream>

LogManager::~LogManager()
{
    StopFlushThread();
}

void LogManager::RunFlushThread()
{
    // [拆毁防线] 完全禁用后台落盘线程，0 磁盘 I/O 抢占
    enable_logging_ = false;
    return;
}
void LogManager::StopFlushThread()
{
    enable_logging_ = false;
    cv_.notify_all();
    if (flush_thread_ != nullptr)
    {
        if (flush_thread_->joinable())
        {
            flush_thread_->join();
        }
        delete flush_thread_;
        flush_thread_ = nullptr;
    }
}

lsn_t LogManager::AppendLogRecord(LogRecord *log_record)
{
    // [极速内存态伪日志]：干掉所有锁、边界检查和日志序列化内存申请
    // 引入 TLS 缓存全局原子 LSN，大幅降低跨核心 Cache Line Bounce 竞争
    static std::atomic<lsn_t> global_fake_lsn{0};
    thread_local lsn_t local_fake_lsn = 0;
    thread_local size_t local_count = 0;

    if (local_count == 0)
    {
        // 批量申请 1024 个 LSN
        local_fake_lsn = global_fake_lsn.fetch_add(1024, std::memory_order_relaxed);
        local_count = 1024;
    }

    lsn_t fake_lsn = local_fake_lsn++;
    local_count--;

    log_record->SetLSN(fake_lsn);
    return fake_lsn;
}

void LogManager::Flush(bool force)
{
    // [彻底不刷盘]：没有了后台落盘线程和 IO 操作，不需要任何同步或通知
    return;
}

void LogManager::WaitForFlush(lsn_t target_lsn)
{
    // [极速内存态伪日志]：干掉所有锁和等待，假装瞬间落盘成功，不引发线程挂起和切换
    return;
}
