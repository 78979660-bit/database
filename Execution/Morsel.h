#pragma once

#include <atomic>
#include <vector>
#include <mutex>
#include <cstdint>
#include <algorithm>

namespace Database {
    struct Morsel {
        size_t start_row{0};
        size_t size{0};
        int numa_node{0};
    };

    class MorselDispatcher {
    public:
        MorselDispatcher() : total_rows_(0), morsel_size_(10000), current_offset_(0) {}
        MorselDispatcher(size_t total_rows, size_t morsel_size = 10000)
            : total_rows_(total_rows), morsel_size_(morsel_size), current_offset_(0) {}

        void Init(size_t total_rows, size_t morsel_size = 10000) {
            total_rows_ = total_rows;
            morsel_size_ = morsel_size;
            current_offset_ = 0;
        }

        bool GetMorsel(Morsel& out_morsel, int thread_numa_node = 0) {
            size_t offset = current_offset_.fetch_add(morsel_size_, std::memory_order_relaxed);
            if (offset >= total_rows_) {
                return false;
            }
            out_morsel.start_row = offset;
            out_morsel.size = std::min(morsel_size_, total_rows_ - offset);
            out_morsel.numa_node = thread_numa_node;
            return true;
        }

        bool IsDone() const {
            return current_offset_.load(std::memory_order_relaxed) >= total_rows_;
        }

    private:
        size_t total_rows_;
        size_t morsel_size_;
        std::atomic<size_t> current_offset_;
    };
}
