#pragma once

#include "LogManager.h"
#include "../BufferPoolManager.h"
#include <unordered_map>
#include <vector>

enum class TransactionStatus
{
    ACTIVE,
    COMMITTED,
    ABORTED
};

struct TransactionState
{
    TransactionStatus status_ = TransactionStatus::ACTIVE;
    lsn_t last_lsn_ = INVALID_LSN;
};

class RecoveryManager {
public:
    RecoveryManager(LogManager *log_manager, BufferPoolManager *buffer_pool_manager) 
        : log_manager_(log_manager), buffer_pool_manager_(buffer_pool_manager) {}

    void ARIES();
    void Checkpoint(); // Fuzzy Checkpoint

private:
    void Analysis();
    void Redo();
    void Undo();

    LogManager *log_manager_;
    BufferPoolManager *buffer_pool_manager_;

    // ATT (Active Transaction Table): maps txn_id to its state
    std::unordered_map<txn_id_t, TransactionState> active_txn_table_;
    
    // DPT (Dirty Page Table): maps page_id to its rec_lsn (the LSN of the first operation that dirtied it)
    std::unordered_map<int32_t, lsn_t> dirty_page_table_;
    
    lsn_t redo_start_lsn_{0};
    std::unordered_map<lsn_t, int> lsn_offset_map_;
};
