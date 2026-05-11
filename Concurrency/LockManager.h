#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <thread>
#include <atomic>
#include "../Common/RID.h"
#include "Transaction.h"

namespace Database
{

    enum class LockMode
    {
        SHARED,
        EXCLUSIVE,
        INTENTION_SHARED,
        INTENTION_EXCLUSIVE,
        SHARED_INTENTION_EXCLUSIVE
    };

    class LockRequest
    {
    public:
        txn_id_t txn_id_;
        LockMode lock_mode_;
        bool granted_;

        LockRequest(txn_id_t txn_id, LockMode lock_mode)
            : txn_id_(txn_id), lock_mode_(lock_mode), granted_(false) {}
    };

    class LockRequestQueue
    {
    public:
        std::vector<std::shared_ptr<LockRequest>> request_queue_;
        std::condition_variable cv_;
        bool upgrading_ = false;
    };

    const size_t NUM_TABLE_LOCK_BUCKETS = 16;

    class alignas(64) TableLockBucket
    {
    public:
        std::mutex latch_;
        std::unordered_map<table_oid_t, std::shared_ptr<LockRequestQueue>> lock_table_;
    };

    const size_t NUM_LOCK_BUCKETS = 64;

    class alignas(64) LockBucket
    {
    public:
        std::mutex latch_;
        std::unordered_map<RID, std::shared_ptr<LockRequestQueue>> lock_table_;
    };

    class LockManager
    {
    public:
        LockManager();
        ~LockManager();

        // Table Lock acquiring and releasing
        bool LockTable(Transaction *txn, LockMode lock_mode, const table_oid_t &oid);
        bool UnlockTable(Transaction *txn, const table_oid_t &oid);

        // Row Lock acquiring and releasing
        bool LockRow(Transaction *txn, LockMode lock_mode, const table_oid_t &oid, const RID &rid);
        bool UnlockRow(Transaction *txn, const table_oid_t &oid, const RID &rid);

        // Backwards compatibility for tests
        bool LockShared(Transaction *txn, const RID &rid) { return LockRow(txn, LockMode::SHARED, 0, rid); }
        bool LockExclusive(Transaction *txn, const RID &rid) { return LockRow(txn, LockMode::EXCLUSIVE, 0, rid); }
        bool Unlock(Transaction *txn, const RID &rid) { return UnlockRow(txn, 0, rid); }

        // Deadlock detection
        void AddEdge(txn_id_t t1, txn_id_t t2);
        void RemoveEdge(txn_id_t t1, txn_id_t t2);
        bool HasCycle(txn_id_t &txn_id);

        // Wait-for graph builder mapping txn wait relationships
        std::unordered_map<txn_id_t, std::vector<txn_id_t>> waits_for_;

    private:
        void RunCycleDetection();
        void BuildWaitsForGraph();
        void AbortTransaction(txn_id_t txn_id);
        bool AreLocksCompatible(LockMode l1, LockMode l2);

        size_t GetBucketIndex(const RID &rid) const
        {
            return std::hash<RID>()(rid) & (NUM_LOCK_BUCKETS - 1);
        }

        size_t GetTableBucketIndex(const table_oid_t &oid) const
        {
            return std::hash<table_oid_t>()(oid) & (NUM_TABLE_LOCK_BUCKETS - 1);
        }

        std::array<TableLockBucket, NUM_TABLE_LOCK_BUCKETS> table_buckets_;
        std::array<LockBucket, NUM_LOCK_BUCKETS> buckets_;
        std::mutex txn_map_latch_;
        std::unordered_map<txn_id_t, Transaction *> txn_map_;

        std::atomic<bool> enable_cycle_detection_;
        std::thread *cycle_detection_thread_;
    };

} // namespace Database
