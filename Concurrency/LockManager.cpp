#include "LockManager.h"
#include <iostream>
#include <algorithm>

namespace Database
{

    LockManager::LockManager() : enable_cycle_detection_(true)
    {
        cycle_detection_thread_ = new std::thread(&LockManager::RunCycleDetection, this);
    }

    LockManager::~LockManager()
    {
        enable_cycle_detection_ = false;
        if (cycle_detection_thread_ && cycle_detection_thread_->joinable())
        {
            cycle_detection_thread_->join();
            delete cycle_detection_thread_;
        }
    }

    bool LockManager::AreLocksCompatible(LockMode l1, LockMode l2) {
        if (l1 == LockMode::INTENTION_SHARED) {
            return l2 == LockMode::INTENTION_SHARED || l2 == LockMode::INTENTION_EXCLUSIVE || l2 == LockMode::SHARED || l2 == LockMode::SHARED_INTENTION_EXCLUSIVE;
        } else if (l1 == LockMode::INTENTION_EXCLUSIVE) {
            return l2 == LockMode::INTENTION_SHARED || l2 == LockMode::INTENTION_EXCLUSIVE;
        } else if (l1 == LockMode::SHARED) {
            return l2 == LockMode::INTENTION_SHARED || l2 == LockMode::SHARED;
        } else if (l1 == LockMode::SHARED_INTENTION_EXCLUSIVE) {
            return l2 == LockMode::INTENTION_SHARED;
        } else {
            return false;
        }
    }

    bool LockManager::LockTable(Transaction *txn, LockMode lock_mode, const table_oid_t &oid)
    {
        size_t bucket_idx = GetTableBucketIndex(oid);
        auto &bucket = table_buckets_[bucket_idx];
        std::unique_lock<std::mutex> lock(bucket.latch_);

        if (txn->GetState() == TransactionState::ABORTED) return false;
        if (txn->GetState() == TransactionState::SHRINKING) {
            txn->SetState(TransactionState::ABORTED);
            // throw TransactionAbortException(txn->GetTransactionId(), AbortReason::LOCK_ON_SHRINKING);
            return false;
        }
        if (txn->GetState() == TransactionState::SHRINKING) {
            txn->SetState(TransactionState::ABORTED);
            // throw TransactionAbortException(txn->GetTransactionId(), AbortReason::LOCK_ON_SHRINKING);
            return false;
        }

        {
            std::lock_guard<std::mutex> txn_lock(txn_map_latch_);
            txn_map_[txn->GetTransactionId()] = txn;
        }

        if (bucket.lock_table_.find(oid) == bucket.lock_table_.end()) {
            bucket.lock_table_[oid] = std::make_shared<LockRequestQueue>();
        }
        auto request_queue = bucket.lock_table_[oid];

        // Ensure we check lock compatibility
        bool wait = false;
        bool upgrade = false;

        // Check if we hold a lock to upgrade
        for (auto it = request_queue->request_queue_.begin(); it != request_queue->request_queue_.end(); ++it) {
            if ((*it)->txn_id_ == txn->GetTransactionId()) {
                if ((*it)->lock_mode_ == lock_mode) return true; // already possess
                // If upgrading, can only upgrade IS->S, IS->IX, IS->SIX, S->X, S->SIX, IX->X, IX->SIX, SIX->X
                if (request_queue->upgrading_) {
                    txn->SetState(TransactionState::ABORTED);
                    return false;
                }
                request_queue->upgrading_ = true;
                (*it)->lock_mode_ = lock_mode;
                (*it)->granted_ = false;
                upgrade = true;
                break;
            }
        }

        if (!upgrade) {
            auto req = std::make_shared<LockRequest>(txn->GetTransactionId(), lock_mode);
            request_queue->request_queue_.push_back(req);
        }

        request_queue->cv_.wait(lock, [&]() {
            if (txn->GetState() == TransactionState::ABORTED) return true;
            bool can_grant = true;
            for (auto &r : request_queue->request_queue_) {
                if (r->txn_id_ == txn->GetTransactionId()) break;
                if (r->granted_ && !AreLocksCompatible(lock_mode, r->lock_mode_)) {
                    can_grant = false;
                    break;
                }
            }
            return can_grant;
        });

        if (txn->GetState() == TransactionState::ABORTED) {
            if (upgrade) request_queue->upgrading_ = false;
            return false;
        }

        if (upgrade) request_queue->upgrading_ = false;

        for (auto &r : request_queue->request_queue_) {
            if (r->txn_id_ == txn->GetTransactionId()) {
                r->granted_ = true;
                break;
            }
        }

        // Attach into Txn
        if (lock_mode == LockMode::SHARED) txn->GetSharedTableLockSet()->insert(oid);
        else if (lock_mode == LockMode::EXCLUSIVE) txn->GetExclusiveTableLockSet()->insert(oid);
        else if (lock_mode == LockMode::INTENTION_SHARED) txn->GetIntentionSharedTableLockSet()->insert(oid);
        else if (lock_mode == LockMode::INTENTION_EXCLUSIVE) txn->GetIntentionExclusiveTableLockSet()->insert(oid);
        else if (lock_mode == LockMode::SHARED_INTENTION_EXCLUSIVE) txn->GetSharedIntentionExclusiveTableLockSet()->insert(oid);

        return true;
    }

    bool LockManager::UnlockTable(Transaction *txn, const table_oid_t &oid)
    {
        if (txn->GetState() == TransactionState::GROWING) {
            txn->SetState(TransactionState::SHRINKING);
        }
        if (txn->GetState() == TransactionState::GROWING) {
            txn->SetState(TransactionState::SHRINKING);
        }
        size_t bucket_idx = GetTableBucketIndex(oid);
        auto &bucket = table_buckets_[bucket_idx];
        std::lock_guard<std::mutex> lock(bucket.latch_);

        txn->GetSharedTableLockSet()->erase(oid);
        txn->GetExclusiveTableLockSet()->erase(oid);
        txn->GetIntentionSharedTableLockSet()->erase(oid);
        txn->GetIntentionExclusiveTableLockSet()->erase(oid);
        txn->GetSharedIntentionExclusiveTableLockSet()->erase(oid);

        if (bucket.lock_table_.find(oid) != bucket.lock_table_.end()) {
            auto request_queue = bucket.lock_table_[oid];
            for (auto it = request_queue->request_queue_.begin(); it != request_queue->request_queue_.end(); ++it) {
                if ((*it)->txn_id_ == txn->GetTransactionId()) {
                    request_queue->request_queue_.erase(it);
                    break;
                }
            }
            request_queue->cv_.notify_all();
        }
        return true;
    }

    bool LockManager::LockRow(Transaction *txn, LockMode lock_mode, const table_oid_t &oid, const RID &rid)
    {
        if (lock_mode == LockMode::INTENTION_SHARED || lock_mode == LockMode::INTENTION_EXCLUSIVE || lock_mode == LockMode::SHARED_INTENTION_EXCLUSIVE) {
            txn->SetState(TransactionState::ABORTED);
            return false;
        }

        size_t bucket_idx = GetBucketIndex(rid);
        auto &bucket = buckets_[bucket_idx];
        std::unique_lock<std::mutex> lock(bucket.latch_);

        if (txn->GetState() == TransactionState::ABORTED) return false;
        if (txn->GetState() == TransactionState::SHRINKING) {
            txn->SetState(TransactionState::ABORTED);
            // throw TransactionAbortException(txn->GetTransactionId(), AbortReason::LOCK_ON_SHRINKING);
            return false;
        }
        if (txn->GetState() == TransactionState::SHRINKING) {
            txn->SetState(TransactionState::ABORTED);
            // throw TransactionAbortException(txn->GetTransactionId(), AbortReason::LOCK_ON_SHRINKING);
            return false;
        }

        {
            std::lock_guard<std::mutex> txn_lock(txn_map_latch_);
            txn_map_[txn->GetTransactionId()] = txn;
        }

        if (bucket.lock_table_.find(rid) == bucket.lock_table_.end()) {
            bucket.lock_table_[rid] = std::make_shared<LockRequestQueue>();
        }
        auto request_queue = bucket.lock_table_[rid];

        bool upgrade = false;
        for (auto it = request_queue->request_queue_.begin(); it != request_queue->request_queue_.end(); ++it) {
            if ((*it)->txn_id_ == txn->GetTransactionId()) {
                if ((*it)->lock_mode_ == lock_mode) return true;
                if (request_queue->upgrading_) {
                    txn->SetState(TransactionState::ABORTED);
                    return false;
                }
                request_queue->upgrading_ = true;
                (*it)->lock_mode_ = lock_mode;
                (*it)->granted_ = false;
                upgrade = true;
                break;
            }
        }

        if (!upgrade) {
            auto req = std::make_shared<LockRequest>(txn->GetTransactionId(), lock_mode);
            request_queue->request_queue_.push_back(req);
        }

        request_queue->cv_.wait(lock, [&]() {
            if (txn->GetState() == TransactionState::ABORTED) return true;
            bool can_grant = true;
            for (auto &r : request_queue->request_queue_) {
                if (r->txn_id_ == txn->GetTransactionId()) break;
                if (r->granted_ && !AreLocksCompatible(lock_mode, r->lock_mode_)) {
                    can_grant = false;
                    break;
                }
            }
            return can_grant;
        });

        if (txn->GetState() == TransactionState::ABORTED) {
            if (upgrade) request_queue->upgrading_ = false;
            return false;
        }

        if (upgrade) {
            request_queue->upgrading_ = false;
            txn->GetSharedLockSet()->erase(rid);
        }

        for (auto &r : request_queue->request_queue_) {
            if (r->txn_id_ == txn->GetTransactionId()) {
                r->granted_ = true;
                break;
            }
        }

        if (lock_mode == LockMode::SHARED) txn->GetSharedLockSet()->insert(rid);
        else txn->GetExclusiveLockSet()->insert(rid);
        return true;
    }

    bool LockManager::UnlockRow(Transaction *txn, const table_oid_t &oid, const RID &rid)
    {
        if (txn->GetState() == TransactionState::GROWING) {
            txn->SetState(TransactionState::SHRINKING);
        }
        if (txn->GetState() == TransactionState::GROWING) {
            txn->SetState(TransactionState::SHRINKING);
        }
        size_t bucket_idx = GetBucketIndex(rid);
        auto &bucket = buckets_[bucket_idx];
        std::lock_guard<std::mutex> lock(bucket.latch_);

        txn->GetSharedLockSet()->erase(rid);
        txn->GetExclusiveLockSet()->erase(rid);

        if (bucket.lock_table_.find(rid) != bucket.lock_table_.end()) {
            auto request_queue = bucket.lock_table_[rid];
            for (auto it = request_queue->request_queue_.begin(); it != request_queue->request_queue_.end(); ++it) {
                if ((*it)->txn_id_ == txn->GetTransactionId()) {
                    request_queue->request_queue_.erase(it);
                    break;
                }
            }
            request_queue->cv_.notify_all();
        }
        return true;
    }

    void LockManager::AddEdge(txn_id_t t1, txn_id_t t2)
    {
        waits_for_[t1].push_back(t2);
    }

    void LockManager::RemoveEdge(txn_id_t t1, txn_id_t t2)
    {
        auto it = std::find(waits_for_[t1].begin(), waits_for_[t1].end(), t2);
        if (it != waits_for_[t1].end())
        {
            waits_for_[t1].erase(it);
        }
    }

    void LockManager::BuildWaitsForGraph()
    {
        waits_for_.clear();
        for (size_t i = 0; i < NUM_TABLE_LOCK_BUCKETS; ++i)
        {
            auto &bucket = table_buckets_[i];
            for (auto &pair : bucket.lock_table_)
            {
                auto queue = pair.second;
                std::vector<txn_id_t> granted_txns;
                for (auto &req : queue->request_queue_) {
                    if (req->granted_) granted_txns.push_back(req->txn_id_);
                    else {
                        for (txn_id_t granted_txn : granted_txns) AddEdge(req->txn_id_, granted_txn);
                    }
                }
            }
        }
        for (size_t i = 0; i < NUM_LOCK_BUCKETS; ++i)
        {
            auto &bucket = buckets_[i];
            for (auto &pair : bucket.lock_table_)
            {
                auto queue = pair.second;
                std::vector<txn_id_t> granted_txns;
                for (auto &req : queue->request_queue_) {
                    if (req->granted_) granted_txns.push_back(req->txn_id_);
                    else {
                        for (txn_id_t granted_txn : granted_txns) AddEdge(req->txn_id_, granted_txn);
                    }
                }
            }
        }
    }

    bool LockManager::HasCycle(txn_id_t &txn_id)
    {
        std::unordered_set<txn_id_t> visited;
        std::unordered_set<txn_id_t> recursion_stack;
        txn_id = INVALID_TXN_ID;

        for (const auto &pair : waits_for_)
        {
            txn_id_t start_node = pair.first;

            auto dfs = [&](auto &self, txn_id_t u) -> bool
            {
                visited.insert(u);
                recursion_stack.insert(u);

                if (waits_for_.find(u) != waits_for_.end())
                {
                    for (txn_id_t v : waits_for_[u])
                    {
                        if (recursion_stack.find(v) != recursion_stack.end())
                        {
                            txn_id = std::max(u, v);
                            return true;
                        }
                        else if (visited.find(v) == visited.end())
                        {
                            if (self(self, v))
                            {
                                txn_id = std::max({txn_id, u, v});
                                return true;
                            }
                        }
                    }
                }

                recursion_stack.erase(u);
                return false;
            };

            if (visited.find(start_node) == visited.end())
            {
                if (dfs(dfs, start_node))
                {
                    return true;
                }
            }
        }
        return false;
    }

    void LockManager::AbortTransaction(txn_id_t txn_id)
    {
        Transaction *txn = nullptr;
        {
            std::lock_guard<std::mutex> txn_lock(txn_map_latch_);
            if (txn_map_.find(txn_id) != txn_map_.end())
            {
                txn = txn_map_[txn_id];
            }
        }

        if (txn)
        {
            txn->SetState(TransactionState::ABORTED);

            for (size_t i = 0; i < NUM_TABLE_LOCK_BUCKETS; ++i)
            {
                auto &bucket = table_buckets_[i];
                for (auto &pair : bucket.lock_table_)
                {
                    pair.second->cv_.notify_all();
                }
            }
            for (size_t i = 0; i < NUM_LOCK_BUCKETS; ++i)
            {
                auto &bucket = buckets_[i];
                for (auto &pair : bucket.lock_table_)
                {
                    pair.second->cv_.notify_all();
                }
            }
        }
    }

    void LockManager::RunCycleDetection()
    {
        while (enable_cycle_detection_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            std::vector<std::unique_lock<std::mutex>> locks;
            for (size_t i = 0; i < NUM_TABLE_LOCK_BUCKETS; ++i)
            {
                locks.emplace_back(table_buckets_[i].latch_);
            }
            for (size_t i = 0; i < NUM_LOCK_BUCKETS; ++i)
            {
                locks.emplace_back(buckets_[i].latch_);
            }

            BuildWaitsForGraph();

            txn_id_t victim_txn_id = INVALID_TXN_ID;
            if (HasCycle(victim_txn_id))
            {
                AbortTransaction(victim_txn_id);
            }
        }
    }

} // namespace Database
