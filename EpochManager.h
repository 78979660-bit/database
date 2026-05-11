#pragma once

#include <atomic>
#include <vector>
#include <functional>
#include <thread>
#include <memory>

/**
 * EpochManager (EBR - Epoch Based Reclamation)
 * 工业级数据库防止锁竞争并在无锁数据结构中安全回收内存的标准方案。
 */
class EpochManager
{
public:
    static constexpr uint64_t MAX_THREADS = 128;

    EpochManager() : global_epoch_(1)
    {
        for (int i = 0; i < MAX_THREADS; ++i)
        {
            local_epochs_[i].epoch.store(0, std::memory_order_relaxed);
        }
    }

    // 线程进入无锁读取区
    void EnterEpoch(int thread_id)
    {
        uint64_t current_epoch = global_epoch_.load(std::memory_order_acquire);
        local_epochs_[thread_id].epoch.store(current_epoch, std::memory_order_release);
    }

    // 线程离开无锁读取区
    void LeaveEpoch(int thread_id)
    {
        local_epochs_[thread_id].epoch.store(0, std::memory_order_release);
    }

    // 推进全局 epoch 并触发回收
    void AdvanceEpoch()
    {
        uint64_t current = global_epoch_.load(std::memory_order_acquire);
        global_epoch_.compare_exchange_strong(current, current + 1);
        ReclaimGarbage();
    }

    // 延迟回收一个对象
    template <typename T>
    void DeferReclaim(T *ptr)
    {
        uint64_t e = global_epoch_.load(std::memory_order_acquire);
        garbage_list_[e % 3].push_back([ptr]()
                                       { delete ptr; });
    }

private:
    void ReclaimGarbage()
    {
        uint64_t current = global_epoch_.load(std::memory_order_acquire);
        uint64_t safe_epoch = current;

        for (int i = 0; i < MAX_THREADS; ++i)
        {
            uint64_t local = local_epochs_[i].epoch.load(std::memory_order_acquire);
            if (local != 0 && local < safe_epoch)
            {
                safe_epoch = local;
            }
        }

        // 清理 safe_epoch 以前注册的垃圾
        uint64_t target = (safe_epoch - 1) % 3;
        for (auto &func : garbage_list_[target])
        {
            func();
        }
        garbage_list_[target].clear();
    }

    std::atomic<uint64_t> global_epoch_;

    // Use an aligned struct to ensure EACH thread's epoch is on a separate cache line
    struct alignas(64) ThreadEpoch
    {
        std::atomic<uint64_t> epoch;
    };
    ThreadEpoch local_epochs_[MAX_THREADS]; // Padding to avoid false sharing

    // 简化的 epoch 垃圾队列
    std::vector<std::function<void()>> garbage_list_[3];
};
