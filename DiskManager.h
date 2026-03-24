#pragma once

#include <atomic>
#include <string>
#include <fstream>
#include <iostream>
#include <filesystem>
#include "Page.h"

/**
 * DiskManager takes care of the allocation and deallocation of pages within a database.
 * It performs the reading and writing of pages to and from disk, providing a logical file layer.
 */
class DiskManager
{
public:
    DiskManager(const std::string &db_file) : file_name_(db_file)
    {
        // Open file in binary read/write mode
        // Create if not exists
        db_io_.open(file_name_, std::ios::binary | std::ios::in | std::ios::out);
        if (!db_io_.is_open())
        {
            db_io_.clear();
            // Try creating a new file
            db_io_.open(file_name_, std::ios::binary | std::ios::trunc | std::ios::out);
            db_io_.close();
            // Reopen in read/write mode
            db_io_.open(file_name_, std::ios::binary | std::ios::in | std::ios::out);
            if (!db_io_.is_open())
            {
                throw std::runtime_error("Can't open valid db file");
            }
        }
    }

    ~DiskManager()
    {
        if (db_io_.is_open())
        {
            db_io_.close();
        }
    }

    /**
     * Write the contents of the specified page into the disk file
     */
    void WritePage(page_id_t page_id, const char *page_data)
    {
        size_t offset = page_id * PAGE_SIZE;
        db_io_.seekp(offset);
        db_io_.write(page_data, PAGE_SIZE);
        if (db_io_.bad())
        {
            std::cerr << "I/O error while writing" << std::endl;
            return;
        }
        db_io_.flush();
    }

    /**
     * Read the contents of the specified page into the given memory area
     */
    void ReadPage(page_id_t page_id, char *page_data)
    {
        size_t offset = page_id * PAGE_SIZE;

        // check if file size is enough, if not, fill with 0
        db_io_.seekg(0, std::ios::end);
        size_t file_size = db_io_.tellg();

        if (offset >= file_size)
        {
            // Reading beyond file, just zero out (or handle error)
            // In real DB, this might be an error or allocating new page implicitly
            memset(page_data, 0, PAGE_SIZE);
        }
        else
        {
            db_io_.seekg(offset);
            db_io_.read(page_data, PAGE_SIZE);
            if (db_io_.bad())
            {
                std::cerr << "I/O error while reading" << std::endl;
                return;
            }
            // if read count < PAGE_SIZE, we can zero out the rest
            size_t read_count = db_io_.gcount();
            if (read_count < PAGE_SIZE)
            {
                memset(page_data + read_count, 0, PAGE_SIZE - read_count);
            }
        }
    }

    /**
     * Allocate a new page ID.
     * For simplicity, this just returns the next page ID based on file size.
     */
    page_id_t AllocatePage()
    {
        // Simple allocation strategy: append to end
        // In reality, we might track free pages with a bitmap
        return next_page_id_++;
    }

    /**
     * Deallocate page (not implemented for simplicity)
     */
    void DeallocatePage(page_id_t page_id)
    {
        // Implementation would mark page as free in bitmap
    }

    int GetFileSize()
    {
        db_io_.seekg(0, std::ios::end);
        return db_io_.tellg();
    }

private:
    std::fstream db_io_;
    std::string file_name_;
    std::atomic<page_id_t> next_page_id_ = 0; // Simple counter for page IDs
};
