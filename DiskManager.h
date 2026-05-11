#pragma once

#include <atomic>
#include <string>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <future>
#include "Page.h"
#include "Recovery/RecoveryDefs.h" // 确保能引用到 lsn_t

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/**
 * DiskManager takes care of the allocation and deallocation of pages within a database.
 * It performs the reading and writing of pages to and from disk, providing a logical file layer.
 */
class DiskManager
{
public:
    DiskManager(const std::string &db_file) : file_name_(db_file)
    {
        log_name_ = file_name_ + ".log";
        master_record_name_ = file_name_ + ".master";
        log_name_ = file_name_ + ".log";
        master_record_name_ = file_name_ + ".master";
#ifdef _WIN32
        db_io_ = CreateFileA(
            file_name_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_ALWAYS,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, // Direct I/O map
            NULL);
        if (db_io_ == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Can't open valid db file with Direct I/O");
        }
#else
        db_io_ = open(file_name_.c_str(), O_RDWR | O_CREAT | O_DIRECT | O_DSYNC, 0666);
        if (db_io_ < 0)
        {
            throw std::runtime_error("Can't open valid db file with Direct I/O");
        }
#endif

        // Initialize page ID counter
#ifdef _WIN32
        LARGE_INTEGER li;
        if (GetFileSizeEx(db_io_, &li))
        {
            next_page_id_ = li.QuadPart / PAGE_SIZE;
        }
#else
        off_t file_size = lseek(db_io_, 0, SEEK_END);
        if (file_size >= 0)
        {
            next_page_id_ = file_size / PAGE_SIZE;
        }
#endif
    }

    ~DiskManager()
    {
#ifdef _WIN32
        if (db_io_ != INVALID_HANDLE_VALUE)
            CloseHandle(db_io_);
#else
        if (db_io_ >= 0)
            close(db_io_);
#endif
        if (log_io_.is_open())
            log_io_.close();
    }

    void WritePage(page_id_t page_id, const char *page_data)
    {
        size_t offset = page_id * PAGE_SIZE;
#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart = offset;
        OVERLAPPED overlapped = {0};
        overlapped.Offset = li.LowPart;
        overlapped.OffsetHigh = li.HighPart;

        DWORD written = 0;
        if (!WriteFile(db_io_, page_data, PAGE_SIZE, &written, &overlapped) && GetLastError() != ERROR_IO_PENDING)
        {
            std::cerr << "I/O error while writing. Error: " << GetLastError() << std::endl;
            return;
        }
        // 同步等待 I/O 完成以兼容现有接口（建议配合 BufferPoolManager 升级为纯异步 Future）
        GetOverlappedResult(db_io_, &overlapped, &written, TRUE);
#else
        if (pwrite(db_io_, page_data, PAGE_SIZE, offset) != PAGE_SIZE)
        {
            std::cerr << "I/O error while writing" << std::endl;
        }
#endif
    }

    // 新增：基于 std::async 封装的跨平台异步写入 (可供后台 Flush 线程调度)
    std::future<bool> WritePageAsync(page_id_t page_id, const char *page_data)
    {
        // 实际工业级应当维护一个内部的 I/O Worker 线程池，或者在 Linux 引入 liburing，在 Windows 引入 IOCP。
        // 此处提供基于 C++11 std::async 标准库实现的最小异步抽象闭包
        return std::async(std::launch::async, [this, page_id, page_data]()
                          {
            this->WritePage(page_id, page_data);
            return true; });
    }

    void ReadPage(page_id_t page_id, char *page_data)
    {
        size_t offset = page_id * PAGE_SIZE;
#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart = offset;
        OVERLAPPED overlapped = {0};
        overlapped.Offset = li.LowPart;
        overlapped.OffsetHigh = li.HighPart;

        DWORD read_count = 0;
        if (!ReadFile(db_io_, page_data, PAGE_SIZE, &read_count, &overlapped) && GetLastError() != ERROR_IO_PENDING)
        {
            memset(page_data, 0, PAGE_SIZE);
        }
        else
        {
            GetOverlappedResult(db_io_, &overlapped, &read_count, TRUE);
            if (read_count < PAGE_SIZE)
            {
                memset(page_data + read_count, 0, PAGE_SIZE - read_count);
            }
        }
#else
        ssize_t read_count = pread(db_io_, page_data, PAGE_SIZE, offset);
        if (read_count < 0)
        {
            std::cerr << "I/O error while reading" << std::endl;
            memset(page_data, 0, PAGE_SIZE);
        }
        else if (read_count < PAGE_SIZE)
        {
            memset(page_data + read_count, 0, PAGE_SIZE - read_count);
        }
#endif
    }

    // 新增：异步读取接口 (避免缺页挂起工作线程)
    std::future<bool> ReadPageAsync(page_id_t page_id, char *page_data)
    {
        return std::async(std::launch::async, [this, page_id, page_data]()
                          {
            this->ReadPage(page_id, page_data);
            return true; });
    }

    page_id_t AllocatePage()
    {
        return next_page_id_++;
    }

    void DeallocatePage(page_id_t page_id) {}

    /**
     * Write log data to the log file.
     */
    void WriteLog(char *log_data, int size)
    {
        if (!log_io_.is_open())
        {
            log_io_.open(log_name_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
            if (!log_io_.is_open())
            {
                // create if not exists
                log_io_.open(log_name_, std::ios::binary | std::ios::trunc | std::ios::out);
                log_io_.close();
                log_io_.open(log_name_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
            }
        }

        log_io_.write(log_data, size);
        log_io_.flush();
    }

    /**
     * Read log data from the log file.
     */
    bool ReadLog(char *log_data, int size, int offset)
    {
        if (!log_io_.is_open())
        {
            log_io_.open(log_name_, std::ios::binary | std::ios::in);
            if (!log_io_.is_open())
                return false;
        }

        log_io_.clear();
        log_io_.seekg(offset);
        log_io_.read(log_data, size);

        if (log_io_.bad())
            return false;
        return log_io_.gcount() == size;
    }

    /**
     * Get the size of the log file.
     */
    int GetLogFileSize()
    {
        if (!log_io_.is_open())
        {
            log_io_.open(log_name_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
            if (!log_io_.is_open())
            {
                log_io_.open(log_name_, std::ios::binary | std::ios::trunc | std::ios::out);
                log_io_.close();
                log_io_.open(log_name_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
            }
        }
        log_io_.clear();
        log_io_.seekg(0, std::ios::end);
        return log_io_.tellg();
    }

    /**
     * Write Master Record LSN
     */
    void UpdateMasterRecord(lsn_t lsn)
    {
        std::ofstream master_io(master_record_name_, std::ios::trunc | std::ios::out | std::ios::binary);
        if (master_io.is_open())
        {
            master_io.write(reinterpret_cast<const char *>(&lsn), sizeof(lsn_t));
            master_io.close();
        }
    }

    /**
     * Read Master Record LSN
     */
    lsn_t GetMasterRecord()
    {
        lsn_t lsn = 0; // Default or INVALID_LSN
        std::ifstream master_io(master_record_name_, std::ios::in | std::ios::binary);
        if (master_io.is_open())
        {
            master_io.read(reinterpret_cast<char *>(&lsn), sizeof(lsn_t));
            master_io.close();
        }
        return lsn;
    }

private:
    std::string file_name_;
#ifdef _WIN32
    HANDLE db_io_;
#else
    int db_io_;
#endif

    std::string log_name_;
    std::string master_record_name_;
    std::fstream log_io_;
    std::atomic<page_id_t> next_page_id_{0};
};
