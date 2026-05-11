#pragma once

#include "TransactionManager.h"
#include "../Catalog/Catalog.h"
#include "EpochManager.h"
#include "EpochManager.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>

namespace Database
{

    class VacuumManager
    {
    public:
        VacuumManager(TransactionManager *txn_mgr, Catalog *catalog)
            : txn_mgr_(txn_mgr), catalog_(catalog), enabled_(true)
        {
            background_thread_ = new std::thread(&VacuumManager::RunVacuumLoop, this);
        }

        ~VacuumManager()
        {
            {
                std::lock_guard<std::mutex> lock(latch_);
                enabled_ = false;
            }
            cv_.notify_all(); // 唤醒后台睡眠线程以实现优雅退出

            if (background_thread_ && background_thread_->joinable())
            {
                background_thread_->join();
                delete background_thread_;
            }
        }

        // 强行立即触发一次后台清理
        void TriggerVacuum()
        {
            cv_.notify_all();
        }

        // 执行一次全表清理
                        void PerformVacuum()
        {
            // Advance global epoch
            EpochManager::GetInstance().AdvanceGlobalEpoch();

            txn_id_t watermark = txn_mgr_->GetWatermark();

            auto table_names = catalog_->GetAllTableNames();
            for (const auto &table_name : table_names)
            {
                TableMetadata *metadata = catalog_->GetTable(table_name);
                if (metadata && metadata->columnar_storage_)
                {
                    metadata->columnar_storage_->Vacuum(watermark);
                }
            }

            // Perform epoch-based garbage collection
            EpochManager::GetInstance().ReclaimProcess();
        }

            // Perform epoch-based garbage collection
            EpochManager::GetInstance().ReclaimProcess();
        }
        }

    private:
        void RunVacuumLoop()
        {
            while (true)
            {
                std::unique_lock<std::mutex> lock(latch_);
                // 如果被外部通知或者达到 5 秒超时，被唤醒
                // 如果退出被标记，则立刻结束
                cv_.wait_for(lock, std::chrono::seconds(5), [this]
                             { return !enabled_; });

                if (!enabled_)
                {
                    break; // 线程安全退出
                }

                // 执行一次清理
                PerformVacuum();
            }
        }

        TransactionManager *txn_mgr_;
        Catalog *catalog_;

        std::mutex latch_;
        std::condition_variable cv_;
        bool enabled_;
        std::thread *background_thread_;
    };

} // namespace Database
