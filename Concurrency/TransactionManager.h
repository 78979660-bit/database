#pragma once

#include "Transaction.h"
#include "../Storage/Table/ColumnarTable.h"
#include "LockManager.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <algorithm>

namespace Database
{

    class TransactionManager
    {
    public:
        TransactionManager(LockManager *lock_manager = nullptr) : next_txn_id_(1), lock_manager_(lock_manager) {}

        ~TransactionManager()
        {
            std::lock_guard<std::mutex> lock(txn_map_mutex_);
            for (auto &pair : active_txns_)
            {
                delete pair.second;
            }
            active_txns_.clear();
        }

        Transaction *Begin(Transaction *txn = nullptr, IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION)
        {
            if (txn != nullptr)
            {
                return txn;
            }

            txn_id_t txn_id = next_txn_id_++;
            Transaction *new_txn = new Transaction(txn_id, isolation_level);

            std::lock_guard<std::mutex> lock(txn_map_mutex_);
            active_txns_[txn_id] = new_txn;

            std::unordered_set<txn_id_t> read_view;
            for (const auto &pair : active_txns_)
            {
                if (pair.first != txn_id)
                {
                    read_view.insert(pair.first);
                }
            }
            new_txn->SetReadView(read_view);

            return new_txn;
        }

        void ReleaseLocks(Transaction *txn)
        {
            if (lock_manager_ == nullptr)
                return;

            std::unordered_set<RID> shared_locks = *txn->GetSharedLockSet();    
            for (const auto &rid : shared_locks)
            {
                lock_manager_->UnlockRow(txn, rid);
            }
            txn->GetSharedLockSet()->clear();

            std::unordered_set<RID> exclusive_locks = *txn->GetExclusiveLockSet();
            for (const auto &rid : exclusive_locks)
            {
                lock_manager_->UnlockRow(txn, rid);
            }
            txn->GetExclusiveLockSet()->clear();

            // Release Table Locks
            std::unordered_set<table_oid_t> s_table_locks = *txn->GetSharedTableLockSet();
            for (const auto &oid : s_table_locks) {
                lock_manager_->UnlockTable(txn, oid);
            }
            txn->GetSharedTableLockSet()->clear();

            std::unordered_set<table_oid_t> x_table_locks = *txn->GetExclusiveTableLockSet();
            for (const auto &oid : x_table_locks) {
                lock_manager_->UnlockTable(txn, oid);
            }
            txn->GetExclusiveTableLockSet()->clear();

            std::unordered_set<table_oid_t> is_table_locks = *txn->GetIntentionSharedTableLockSet();
            for (const auto &oid : is_table_locks) {
                lock_manager_->UnlockTable(txn, oid);
            }
            txn->GetIntentionSharedTableLockSet()->clear();

            std::unordered_set<table_oid_t> ix_table_locks = *txn->GetIntentionExclusiveTableLockSet();
            for (const auto &oid : ix_table_locks) {
                lock_manager_->UnlockTable(txn, oid);
            }
            txn->GetIntentionExclusiveTableLockSet()->clear();

            std::unordered_set<table_oid_t> six_table_locks = *txn->GetSharedIntentionExclusiveTableLockSet();
            for (const auto &oid : six_table_locks) {
                lock_manager_->UnlockTable(txn, oid);
            }
            txn->GetSharedIntentionExclusiveTableLockSet()->clear();
        }

        void Commit(Transaction *txn)
        {
            txn->SetState(TransactionState::COMMITTED);

            ReleaseLocks(txn);

            std::lock_guard<std::mutex> lock(txn_map_mutex_);
            active_txns_.erase(txn->GetTransactionId());
        }

        void Abort(Transaction *txn)
        {
            txn->SetState(TransactionState::ABORTED);

            // ================== ����ع� ==================
            const auto &write_set = txn->GetWriteSet();
            // ���뷴�����(LIFO)����ֹͬһ�������ͬһ�ж�β���������������
            for (auto it = write_set.rbegin(); it != write_set.rend(); ++it)
            {
                const auto &record = *it;
                ColumnarTable *table = record.table_;
                RID rid = record.rid_;

                Tuple temp_tuple;
                if (!table->GetTuple(rid, &temp_tuple))
                {
                    continue; // ��������ҳ�𻵻��߱� Vacuum �������Ȼ���÷���
                }

                TupleMeta meta = temp_tuple.GetMeta();

                if (record.wtype_ == WType::W_DELETE)
                {
                    // �����߼�ɾ��������ԭ���Ǳ������� delete ������Լ��� ID
                    // �������ǰ����ñ�־���ָ�ԭ״
                    meta.delete_txn_id_ = INVALID_TXN_ID;
                    table->UpdateTuple(meta, rid);
                }
                else if (record.wtype_ == WType::W_INSERT)
                {
                    // ��������(�Լ�Updateʱ���²岿��)��
                    // ���м�¼�Ͳ��ñ������˿���������ֱ������αɾ����(��δ����Vacuum��¶)
                    meta.is_deleted_ = true;
                    table->UpdateTuple(meta, rid);
                }
                // UPDATE ����������Ŀǰ�� "Delete old + Insert new" ģ���У������� 1�� DELETE + 1�� INSERT ����������ֻ������������֧���ܴ���
            }
            // ==============================================

            ReleaseLocks(txn);

            std::lock_guard<std::mutex> lock(txn_map_mutex_);
            active_txns_.erase(txn->GetTransactionId());
        }

        txn_id_t GetWatermark()
        {
            std::lock_guard<std::mutex> lock(txn_map_mutex_);
            txn_id_t watermark = next_txn_id_;
            for (const auto &pair : active_txns_)
            {
                if (pair.first < watermark)
                {
                    watermark = pair.first;
                }
            }
            return watermark;
        }

    private:
        std::atomic<txn_id_t> next_txn_id_;
        std::mutex txn_map_mutex_;
        std::unordered_map<txn_id_t, Transaction *> active_txns_;
        LockManager *lock_manager_;
    };

} // namespace Database
