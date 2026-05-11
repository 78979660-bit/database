#pragma once

#include <mutex>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <limits>

using frame_id_t = int;

/**
 * Replacer is an abstract base class for replacement policies (e.g. LRU, LFU, Clock).
 */
class Replacer
{
public:
    virtual ~Replacer() = default;

    virtual bool Victim(frame_id_t *frame_id) = 0;

    virtual void Pin(frame_id_t frame_id) = 0;

    virtual void Unpin(frame_id_t frame_id) = 0;

    virtual void RecordAccess(frame_id_t frame_id) {}

    virtual void Remove(frame_id_t frame_id) {}

    virtual size_t Size() = 0;
};

/**
 * ClockSweepReplacer implements an industrial-strength Clock-Sweep replacement policy
 * with scan-resistance (similar to PostgreSQL's buffer replacement).
 * Time complexity per operation is strictly O(1) amortized, avoiding full-table scans.
 */
class ClockSweepReplacer : public Replacer
{
public:
    explicit ClockSweepReplacer(size_t num_pages, int max_usage_count = 5)
        : capacity_(num_pages), max_usage_count_(max_usage_count), clock_hand_(0), current_size_(0)
    {
        frame_entries_.resize(num_pages);
        for (size_t i = 0; i < num_pages; ++i)
        {
            frame_entries_[i].usage_count = 0;
            frame_entries_[i].is_pinned = true; // By default, unused frames act as pinned to avoid eviction
            frame_entries_[i].in_replacer = false;
        }
    }

    bool Victim(frame_id_t *frame_id) override
    {
        // [B方案: 剥离全共享结构] std::lock_guard<std::mutex> lock(mutex_); // 不再锁 Replacer
        if (current_size_ == 0)
        {
            return false;
        }

        while (true)
        {
            auto &entry = frame_entries_[clock_hand_];
            if (entry.in_replacer && !entry.is_pinned)
            {
                if (entry.usage_count > 0)
                {
                    // Decrement usage count (give it a second chance)
                    entry.usage_count--;
                }
                else
                {
                    // Evict this frame
                    entry.in_replacer = false;
                    current_size_--;
                    *frame_id = clock_hand_;
                    clock_hand_++;
                    if (clock_hand_ == capacity_)
                    {
                        clock_hand_ = 0;
                    }
                    return true;
                }
            }
            clock_hand_++;
            if (clock_hand_ == capacity_)
            {
                clock_hand_ = 0;
            }
        }
    }

    void Pin(frame_id_t frame_id) override
    {
        // [B方案: 剥离全共享结构] std::lock_guard<std::mutex> lock(mutex_); // 不再锁 Replacer
        if (frame_entries_[frame_id].in_replacer && !frame_entries_[frame_id].is_pinned)
        {
            frame_entries_[frame_id].is_pinned = true;
            current_size_--;
        }
    }

    void Unpin(frame_id_t frame_id) override
    {
        // [B方案: 剥离全共享结构] std::lock_guard<std::mutex> lock(mutex_); // 不再锁 Replacer
        if (!frame_entries_[frame_id].in_replacer || frame_entries_[frame_id].is_pinned)
        {
            frame_entries_[frame_id].is_pinned = false;
            if (!frame_entries_[frame_id].in_replacer)
            {
                frame_entries_[frame_id].in_replacer = true;
                current_size_++;
            }
        }
    }

    void RecordAccess(frame_id_t frame_id) override
    {
        // [B方案: 剥离全共享结构] std::lock_guard<std::mutex> lock(mutex_); // 不再锁 Replacer
        if (frame_entries_[frame_id].usage_count < max_usage_count_)
        {
            frame_entries_[frame_id].usage_count++;
        }
    }

    void Remove(frame_id_t frame_id) override
    {
        // [B方案: 剥离全共享结构] std::lock_guard<std::mutex> lock(mutex_); // 不再锁 Replacer
        if (frame_entries_[frame_id].in_replacer && !frame_entries_[frame_id].is_pinned)
        {
            frame_entries_[frame_id].in_replacer = false;
            frame_entries_[frame_id].usage_count = 0;
            current_size_--;
        }
    }

    size_t Size() override
    {
        // [B方案: 剥离全共享结构] std::lock_guard<std::mutex> lock(mutex_); // 不再锁 Replacer
        return current_size_;
    }

private:
    struct alignas(64) FrameEntry
    {
        int usage_count;
        bool is_pinned;
        bool in_replacer;
    };

    std::vector<FrameEntry> frame_entries_;
    size_t capacity_;
    int max_usage_count_;
    size_t clock_hand_;
    size_t current_size_;
    std::mutex mutex_;
};
