#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_set>
#include <vector>
#include "../Common/RID.h"

#undef DELETE
#undef UPDATE
#undef INSERT

namespace Database
{

    using txn_id_t = uint64_t;
    constexpr txn_id_t INVALID_TXN_ID = 0;

    enum class TransactionState
    {
        GROWING,
        SHRINKING,
        COMMITTED,
        ABORTED
    };

    enum class IsolationLevel
    {
        READ_UNCOMMITTED,
        READ_COMMITTED,
        REPEATABLE_READ,
        SNAPSHOT_ISOLATION,
        SERIALIZABLE
    };

    class ColumnarTable; // ǰ������

    enum class WType
    {
        W_INSERT,
        W_DELETE,
        W_UPDATE
    };

    // ��¼ÿһ���޸���Ϊ�����ڻع�
    struct WriteRecord
    {
        RID rid_;
        WType wtype_;
        ColumnarTable *table_;
    };

    class Transaction
    {
    public:
        explicit Transaction(txn_id_t txn_id, IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION)
            : txn_id_(txn_id), state_(TransactionState::GROWING), isolation_level_(isolation_level) {}

        ~Transaction() = default;

        txn_id_t GetTransactionId() const { return txn_id_; }

        TransactionState GetState() const { return state_; }
        void SetState(TransactionState state) { state_ = state; }

        IsolationLevel GetIsolationLevel() const { return isolation_level_; }

        void SetReadView(const std::unordered_set<txn_id_t> &active_txns)
        {
            read_view_txns_ = active_txns;
        }

        bool IsTxnActive(txn_id_t other_txn_id) const
        {
            return read_view_txns_.find(other_txn_id) != read_view_txns_.end();
        }

        // ================== Undo ��� ======================
        void AppendWriteRecord(const WriteRecord &record)
        {
            write_set_.push_back(record);
        }

        const std::vector<WriteRecord> &GetWriteSet() const
        {
            return write_set_;
        }
        // ===================================================


        std::shared_ptr<std::unordered_set<table_oid_t>> GetSharedTableLockSet() const { return shared_table_lock_set_; }
        std::shared_ptr<std::unordered_set<table_oid_t>> GetExclusiveTableLockSet() const { return exclusive_table_lock_set_; }
        std::shared_ptr<std::unordered_set<table_oid_t>> GetIntentionSharedTableLockSet() const { return intention_shared_table_lock_set_; }
        std::shared_ptr<std::unordered_set<table_oid_t>> GetIntentionExclusiveTableLockSet() const { return intention_exclusive_table_lock_set_; }
        std::shared_ptr<std::unordered_set<table_oid_t>> GetSharedIntentionExclusiveTableLockSet() const { return shared_intention_exclusive_table_lock_set_; }

        std::shared_ptr<std::unordered_set<RID>> GetSharedLockSet() const { return shared_lock_set_; }

        std::shared_ptr<std::unordered_set<RID>> GetExclusiveLockSet() const { return exclusive_lock_set_; }

    private:
        txn_id_t txn_id_;
        TransactionState state_;
        IsolationLevel isolation_level_;

        std::unordered_set<txn_id_t> read_view_txns_;

        std::vector<WriteRecord> write_set_;


        std::shared_ptr<std::unordered_set<RID>> shared_lock_set_ = std::make_shared<std::unordered_set<RID>>();
        std::shared_ptr<std::unordered_set<RID>> exclusive_lock_set_ = std::make_shared<std::unordered_set<RID>>();

        std::shared_ptr<std::unordered_set<table_oid_t>> shared_table_lock_set_ = std::make_shared<std::unordered_set<table_oid_t>>();
        std::shared_ptr<std::unordered_set<table_oid_t>> exclusive_table_lock_set_ = std::make_shared<std::unordered_set<table_oid_t>>();
        std::shared_ptr<std::unordered_set<table_oid_t>> intention_shared_table_lock_set_ = std::make_shared<std::unordered_set<table_oid_t>>();
        std::shared_ptr<std::unordered_set<table_oid_t>> intention_exclusive_table_lock_set_ = std::make_shared<std::unordered_set<table_oid_t>>();
        std::shared_ptr<std::unordered_set<table_oid_t>> shared_intention_exclusive_table_lock_set_ = std::make_shared<std::unordered_set<table_oid_t>>();
    };


} // namespace Database
