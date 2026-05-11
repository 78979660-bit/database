#pragma once

#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <array>

namespace Database {

    // Helper structure to hold deferred garbage elements
    struct DeferredAction {
        uint64_t epoch;
        std::function<void()> action;
    };

    /**
     * @brief EpochManager implements Epoch-based Reclamation (EBR).
     * This provides low-latency grace period garbage collection for MVCC 
     * versions and concurrent index nodes.
     */
    class EpochManager {
    public:
        EpochManager() : global_epoch_(0) {}

        // Global singleton
        static EpochManager& GetInstance() {
            static EpochManager instance;
            return instance;
        }

        // Thread hooks to enter and leave epochs
        void EnterEpoch(int thread_id) {
            local_epochs_[thread_id].store(global_epoch_.load(std::memory_order_relaxed), std::memory_order_release);
        }

        void ExitEpoch(int thread_id) {
            // UINT64_MAX means not active
            local_epochs_[thread_id].store(UINT64_MAX, std::memory_order_release);
        }

        // Advance global epoch periodically
        void AdvanceGlobalEpoch() {
            global_epoch_.fetch_add(1, std::memory_order_relaxed);
        }

        // Defer an action (like freeing memory) to be executed when safe
        void DeferAction(std::function<void()> action) {
            std::lock_guard<std::mutex> lock(action_mutex_);
            garbage_.push_back({global_epoch_.load(std::memory_order_relaxed), std::move(action)});
        }

        // Reclaim garbage that is older than the minimum active local epoch
        void ReclaimProcess() {
            uint64_t min_active_epoch = global_epoch_.load(std::memory_order_relaxed);
            for (auto& le : local_epochs_) {
                uint64_t val = le.store_val.load(std::memory_order_acquire);
                if (val != UINT64_MAX && val < min_active_epoch) {
                    min_active_epoch = val;
                }
            }

            std::vector<DeferredAction> remaining;
            std::vector<std::function<void()>> to_free;

            {
                std::lock_guard<std::mutex> lock(action_mutex_);
                for (auto& item : garbage_) {
                    if (item.epoch < min_active_epoch) {
                        to_free.push_back(std::move(item.action));
                    } else {
                        remaining.push_back(std::move(item));
                    }
                }
                garbage_ = std::move(remaining);
            }

            for (auto& action : to_free) {
                action();
            }
        }

    private:
        struct alignas(64) PaddedAtomic { // Prevents false sharing
            std::atomic<uint64_t> store_val{UINT64_MAX};
        };

        std::atomic<uint64_t> global_epoch_;
        std::array<PaddedAtomic, 128> local_epochs_; // Support up to 128 threads

        std::mutex action_mutex_;
        std::vector<DeferredAction> garbage_;
    };

}
