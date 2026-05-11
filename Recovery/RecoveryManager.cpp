#include "RecoveryManager.h"
#include <algorithm>
#include <iostream>
#include <cassert>

void RecoveryManager::ARIES()
{
    Analysis();
    Redo();
    Undo();
}

void RecoveryManager::Checkpoint()
{
    LogRecord begin_record(INVALID_TXN_ID, INVALID_LSN, LogRecordType::CHECKPOINT_BEGIN);
    lsn_t begin_lsn = log_manager_->AppendLogRecord(&begin_record);

    std::unordered_map<txn_id_t, lsn_t> current_att;
    for (auto &p : active_txn_table_)
    {
        current_att[p.first] = p.second.last_lsn_;
    }
    std::unordered_map<page_id_t, lsn_t> current_dpt = dirty_page_table_;

    LogRecord end_record(INVALID_TXN_ID, INVALID_LSN, LogRecordType::CHECKPOINT_END);
    end_record.att_ = current_att;
    end_record.dpt_ = current_dpt;
    log_manager_->AppendLogRecord(&end_record);

    if (log_manager_->GetDiskManager() != nullptr)
    {
        log_manager_->GetDiskManager()->UpdateMasterRecord(begin_lsn);
    }

    log_manager_->Flush(true);
}

void RecoveryManager::Analysis()
{
    std::cout << "--- [ARIES] Starting Analysis Phase ---\n";
    // Start scanning from MasterRecord (last Checkpoint Begin) or 0
    int offset = 0;

    int file_size = log_manager_->GetDiskManager()->GetLogFileSize();
    if (file_size == 0)
        return;

    while (offset < file_size)
    {
        // First read size (assuming 4 bytes size at the beginning of each record)
        int32_t size = 0;
        if (!log_manager_->GetDiskManager()->ReadLog(reinterpret_cast<char *>(&size), sizeof(int32_t), offset))
        {
            break; // EOF or error
        }

        if (size <= 0 || offset + size > file_size)
        {
            break; // Corrupted or incomplete log
        }

        std::vector<char> buffer(size);
        log_manager_->GetDiskManager()->ReadLog(buffer.data(), size, offset);

        LogRecord log = LogRecord::Deserialize(buffer.data());
        lsn_offset_map_[log.GetLSN()] = offset;
        std::cout << "[Analysis] Read LSN: " << log.GetLSN() << " Txn: " << log.GetTxnId() << " Type: " << static_cast<int>(log.GetLogRecordType()) << std::endl;

        // Apply logic to rebuild ATT and DPT
        switch (log.GetLogRecordType())
        {
        case LogRecordType::BEGIN:
            active_txn_table_[log.GetTxnId()] = {TransactionStatus::ACTIVE, log.GetLSN()};
            break;
        case LogRecordType::COMMIT:
        case LogRecordType::ABORT:
            active_txn_table_.erase(log.GetTxnId());
            break;
        case LogRecordType::UPDATE:
        case LogRecordType::INSERT:
        case LogRecordType::APPLY_DELETE:
        case LogRecordType::MARK_TOMBSTONE:
        case LogRecordType::CLR:
            active_txn_table_[log.GetTxnId()] = {TransactionStatus::ACTIVE, log.GetLSN()};
            if (dirty_page_table_.find(log.page_id_) == dirty_page_table_.end())
            {
                dirty_page_table_[log.page_id_] = log.GetLSN();
            }
            break;
        case LogRecordType::CHECKPOINT_END:
            // Incorporate ATT and DPT from checkpoint
            for (auto &p : log.att_)
            {
                active_txn_table_[p.first] = {TransactionStatus::ACTIVE, p.second};
            }
            for (auto &p : log.dpt_)
            {
                if (dirty_page_table_.find(p.first) == dirty_page_table_.end())
                {
                    dirty_page_table_[p.first] = p.second;
                }
                else
                {
                    dirty_page_table_[p.first] = std::min(dirty_page_table_[p.first], p.second);
                }
            }
            break;
        default:
            break;
        }

        offset += size;
    }

    lsn_t min_rec_lsn = INVALID_LSN;
    for (auto const &p : dirty_page_table_)
    {
        if (min_rec_lsn == INVALID_LSN || p.second < min_rec_lsn)
        {
            min_rec_lsn = p.second;
        }
    }
    redo_start_lsn_ = (min_rec_lsn == INVALID_LSN) ? 0 : min_rec_lsn;
}

void RecoveryManager::Redo()
{
    std::cout << "--- [ARIES] Starting Redo Phase ---\n";
    


    if (redo_start_lsn_ == INVALID_LSN || lsn_offset_map_.find(redo_start_lsn_) == lsn_offset_map_.end())
    {
        return;
    }

    int offset = lsn_offset_map_[redo_start_lsn_];
    int file_size = log_manager_->GetDiskManager()->GetLogFileSize();

    while (offset < file_size)
    {
        int32_t size = 0;
        if (!log_manager_->GetDiskManager()->ReadLog(reinterpret_cast<char *>(&size), sizeof(int32_t), offset)) {
            break;
        }
        if (size <= 0 || offset + size > file_size)
            break;

        std::vector<char> buffer(size);
        log_manager_->GetDiskManager()->ReadLog(buffer.data(), size, offset);

        LogRecord log = LogRecord::Deserialize(buffer.data());

        // History repeats itself
        if (log.GetLogRecordType() == LogRecordType::UPDATE || log.GetLogRecordType() == LogRecordType::INSERT ||
            log.GetLogRecordType() == LogRecordType::APPLY_DELETE || log.GetLogRecordType() == LogRecordType::MARK_TOMBSTONE ||
            log.GetLogRecordType() == LogRecordType::CLR)
        {

            if (dirty_page_table_.find(log.page_id_) != dirty_page_table_.end() &&
                log.GetLSN() >= dirty_page_table_[log.page_id_])
            {

                Page *page = buffer_pool_manager_->FetchPage(log.page_id_);
                if (page != nullptr)
                {
                    if (page->GetLSN() < log.GetLSN())
                    {
                        // Apply REDO: Write after_image_ to the page
                        if (!log.after_image_.empty())
                        {
                            memcpy(page->GetData() + log.offset_, log.after_image_.data(), log.after_image_.size());
                        }
                        page->SetLSN(log.GetLSN());
                    }
                    buffer_pool_manager_->UnpinPage(log.page_id_, true);
                }
            }
        }

        offset += size;
    }
}

void RecoveryManager::Undo()
{
    std::cout << "--- [ARIES] Starting Undo Phase ---\n";
    std::vector<lsn_t> undo_list;
    for (auto const &p : active_txn_table_)
    {
        if (p.second.status_ == TransactionStatus::ACTIVE && p.second.last_lsn_ != INVALID_LSN)
        {
            undo_list.push_back(p.second.last_lsn_);
        }
    }

    // Process backwards: Always pick the max LSN to undo.
    while (!undo_list.empty())
    {
        auto it = std::max_element(undo_list.begin(), undo_list.end());
        lsn_t curr_lsn = *it;
        undo_list.erase(it);

        if (lsn_offset_map_.find(curr_lsn) == lsn_offset_map_.end())
            continue;

        int offset = lsn_offset_map_[curr_lsn];
        int file_size = log_manager_->GetDiskManager()->GetLogFileSize();

        int32_t size = 0;
        log_manager_->GetDiskManager()->ReadLog(reinterpret_cast<char *>(&size), sizeof(int32_t), offset);

        std::vector<char> buffer(size);
        log_manager_->GetDiskManager()->ReadLog(buffer.data(), size, offset);
        LogRecord log = LogRecord::Deserialize(buffer.data());

        // Reverse the operation of UPDATE/INSERT/DELETE
        if (log.GetLogRecordType() == LogRecordType::UPDATE || log.GetLogRecordType() == LogRecordType::INSERT ||
            log.GetLogRecordType() == LogRecordType::APPLY_DELETE || log.GetLogRecordType() == LogRecordType::MARK_TOMBSTONE)
        {
            Page *page = buffer_pool_manager_->FetchPage(log.page_id_);
            if (page != nullptr)
            {
                // Apply UNDO (Compensation): restore the before_image
                if (!log.before_image_.empty())
                {
                    std::cout << "[Undo] Applying before image of size " << log.before_image_.size() << " to page " << log.page_id_ << std::endl;
                    memcpy(page->GetData() + log.offset_, log.before_image_.data(), log.before_image_.size());
                }
                else
                {
                    std::cout << "[Undo] Warning: before_image is empty for page " << log.page_id_ << std::endl;
                }

                // Write a CLR (Compensation Log Record)
                lsn_t current_last_lsn = active_txn_table_[log.GetTxnId()].last_lsn_;

                // The CLR redo payload is the before_image of the aborted operation
                LogRecord clr(log.GetTxnId(), current_last_lsn, LogRecordType::CLR, log.page_id_, log.offset_, "", log.before_image_);
                clr.undo_next_lsn_ = log.prev_lsn_; // Crucial for ARIES
                lsn_t clr_lsn = log_manager_->AppendLogRecord(&clr);

                // Update txn's last_lsn to the CLR
                active_txn_table_[log.GetTxnId()].last_lsn_ = clr_lsn;

                // Update the page LSN
                page->SetLSN(clr_lsn);
                buffer_pool_manager_->UnpinPage(log.page_id_, true);
            }

            // Move to the previous log record for this transaction
            if (log.prev_lsn_ != INVALID_LSN)
            {
                undo_list.push_back(log.prev_lsn_);
            }
        }
        else if (log.GetLogRecordType() == LogRecordType::CLR)
        {
            // If it's a CLR, we crashed during a previous Undo phase!
            // Do NOT undo the CLR. Instead, skip directly to its undo_next_lsn_
            if (log.undo_next_lsn_ != INVALID_LSN)
            {
                undo_list.push_back(log.undo_next_lsn_);
            }
        }
        else
        {
            // For BEGIN, ABORT, COMMIT, etc. just follow prev_lsn
            if (log.prev_lsn_ != INVALID_LSN)
            {
                undo_list.push_back(log.prev_lsn_);
            }
        }
    }

    // After Undo, abort all remaining transactions (write ABORT record, remove from ATT)
    for (auto &p : active_txn_table_)
    {
        LogRecord abort_log(p.first, p.second.last_lsn_, LogRecordType::ABORT);
        log_manager_->AppendLogRecord(&abort_log);
    }
    active_txn_table_.clear();

    // Group Commit trigger to ensure all CLR and ABORT records are persisted
    log_manager_->Flush(true);
}
