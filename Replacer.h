#pragma once

#include <mutex>
#include <list>
#include <unordered_map>
#include <vector>

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

    virtual size_t Size() = 0;
};

/**
 * LRUReplacer implements the Least Recently Used replacement policy.
 */
class LRUReplacer : public Replacer
{
public:
    LRUReplacer(size_t num_pages) {}

    ~LRUReplacer() override = default;

    bool Victim(frame_id_t *frame_id) override
    {
        std::scoped_lock lock{mutex_};
        if (lru_list_.empty())
        {
            return false;
        }
        // Remove from back (least recently used)
        *frame_id = lru_list_.back();
        lru_map_.erase(*frame_id);
        lru_list_.pop_back();
        return true;
    }

    void Pin(frame_id_t frame_id) override
    {
        std::scoped_lock lock{mutex_};
        auto it = lru_map_.find(frame_id);
        if (it != lru_map_.end())
        {
            lru_list_.erase(it->second);
            lru_map_.erase(it);
        }
    }

    void Unpin(frame_id_t frame_id) override
    {
        std::scoped_lock lock{mutex_};
        if (lru_map_.find(frame_id) != lru_map_.end())
        {
            return;
        }
        // Add to front (most recently used)
        lru_list_.push_front(frame_id);
        lru_map_[frame_id] = lru_list_.begin();
    }

    size_t Size() override
    {
        std::scoped_lock lock{mutex_};
        return lru_list_.size();
    }

private:
    std::mutex mutex_;
    std::list<frame_id_t> lru_list_;
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> lru_map_;
};
