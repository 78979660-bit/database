#pragma once

#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <cassert>
#include "RecoveryDefs.h"

// For simplicity, we define a fixed-size header format.
// Header format:
// 0: size_ (int32_t)
// 4: lsn_ (int32_t)
// 8: txn_id_ (int32_t)
// 12: prev_lsn_ (int32_t)
// 16: log_record_type_ (int32_t)

class LogRecord
{
public:
    static const int HEADER_SIZE = 20;

    LogRecord() = default;

    // Default constructor for control records
    LogRecord(txn_id_t txn_id, lsn_t prev_lsn, LogRecordType log_record_type)
        : size_(HEADER_SIZE), txn_id_(txn_id), prev_lsn_(prev_lsn), log_record_type_(log_record_type)
    {
        if (log_record_type_ == LogRecordType::CLR)
        {
            size_ += 12; // 8 for page_id & offset, 4 for undo_next_lsn
            size_ += 8;  // Two 4-byte lengths for empty before/after images
        }
    }

    // Constructor for data modification records
    LogRecord(txn_id_t txn_id, lsn_t prev_lsn, LogRecordType log_record_type, int32_t page_id, int32_t offset = 0, const std::string &before_image = "", const std::string &after_image = "")
        : txn_id_(txn_id), prev_lsn_(prev_lsn), log_record_type_(log_record_type),
          page_id_(page_id), offset_(offset), before_image_(before_image), after_image_(after_image)
    {
        size_ = HEADER_SIZE;
        if (log_record_type_ == LogRecordType::INSERT || log_record_type_ == LogRecordType::UPDATE ||
            log_record_type_ == LogRecordType::APPLY_DELETE || log_record_type_ == LogRecordType::MARK_TOMBSTONE || log_record_type_ == LogRecordType::CLR)
        {
            size_ += 8;                        // page_id, offset
            size_ += 4 + before_image_.size(); // len + data
            size_ += 4 + after_image_.size();  // len + data
        }

        if (log_record_type_ == LogRecordType::CLR)
        {
            size_ += 4; // Add 4 for undo_next_lsn_
        }
    }
    lsn_t lsn_ = INVALID_LSN;
    txn_id_t txn_id_ = INVALID_TXN_ID;
    lsn_t prev_lsn_ = INVALID_LSN;
    LogRecordType log_record_type_ = LogRecordType::INVALID;
    int32_t size_ = 0;

    int32_t page_id_ = -1;
    int32_t offset_ = -1;
    std::string before_image_;
    std::string after_image_;
    lsn_t undo_next_lsn_ = INVALID_LSN;

    std::unordered_map<txn_id_t, lsn_t> att_;
    std::unordered_map<page_id_t, lsn_t> dpt_;

    lsn_t GetLSN() const { return lsn_; }
    void SetLSN(lsn_t lsn) { lsn_ = lsn; }
    txn_id_t GetTxnId() const { return txn_id_; }
    lsn_t GetPrevLSN() const { return prev_lsn_; }
    LogRecordType GetLogRecordType() const { return log_record_type_; }
    int32_t GetSize() const { return size_; }

    void Serialize(char *dest) const
    {
        int32_t current_offset = 0;
        memcpy(dest + current_offset, &size_, sizeof(int32_t));
        current_offset += sizeof(int32_t);
        memcpy(dest + current_offset, &lsn_, sizeof(int32_t));
        current_offset += sizeof(int32_t);
        memcpy(dest + current_offset, &txn_id_, sizeof(int32_t));
        current_offset += sizeof(int32_t);
        memcpy(dest + current_offset, &prev_lsn_, sizeof(int32_t));
        current_offset += sizeof(int32_t);
        int32_t type = static_cast<int32_t>(log_record_type_);
        memcpy(dest + current_offset, &type, sizeof(int32_t));
        current_offset += sizeof(int32_t);

        if (log_record_type_ == LogRecordType::INSERT || log_record_type_ == LogRecordType::UPDATE ||
            log_record_type_ == LogRecordType::APPLY_DELETE || log_record_type_ == LogRecordType::MARK_TOMBSTONE || log_record_type_ == LogRecordType::CLR)
        {
            memcpy(dest + current_offset, &page_id_, sizeof(int32_t));
            current_offset += sizeof(int32_t);
            memcpy(dest + current_offset, &offset_, sizeof(int32_t));
            current_offset += sizeof(int32_t);

            // Serialize before_image_
            int32_t before_len = before_image_.size();
            memcpy(dest + current_offset, &before_len, sizeof(int32_t));
            current_offset += sizeof(int32_t);
            if (before_len > 0)
            {
                memcpy(dest + current_offset, before_image_.data(), before_len);
                current_offset += before_len;
            }

            // Serialize after_image_
            int32_t after_len = after_image_.size();
            memcpy(dest + current_offset, &after_len, sizeof(int32_t));
            current_offset += sizeof(int32_t);
            if (after_len > 0)
            {
                memcpy(dest + current_offset, after_image_.data(), after_len);
                current_offset += after_len;
            }
        }

        if (log_record_type_ == LogRecordType::CLR)
        {
            memcpy(dest + current_offset, &undo_next_lsn_, sizeof(int32_t));
            current_offset += sizeof(int32_t);
        }
    }

    static LogRecord Deserialize(const char *data)
    {
        LogRecord rec;
        int32_t current_offset = 0;
        memcpy(&rec.size_, data + current_offset, sizeof(int32_t));
        current_offset += sizeof(int32_t);
        memcpy(&rec.lsn_, data + current_offset, sizeof(int32_t));
        current_offset += sizeof(int32_t);
        memcpy(&rec.txn_id_, data + current_offset, sizeof(int32_t));
        current_offset += sizeof(int32_t);
        memcpy(&rec.prev_lsn_, data + current_offset, sizeof(int32_t));
        current_offset += sizeof(int32_t);
        int32_t type;
        memcpy(&type, data + current_offset, sizeof(int32_t));
        current_offset += sizeof(int32_t);
        rec.log_record_type_ = static_cast<LogRecordType>(type);

        if (rec.log_record_type_ == LogRecordType::INSERT || rec.log_record_type_ == LogRecordType::UPDATE ||
            rec.log_record_type_ == LogRecordType::APPLY_DELETE || rec.log_record_type_ == LogRecordType::MARK_TOMBSTONE || rec.log_record_type_ == LogRecordType::CLR)
        {
            memcpy(&rec.page_id_, data + current_offset, sizeof(int32_t));
            current_offset += sizeof(int32_t);
            memcpy(&rec.offset_, data + current_offset, sizeof(int32_t));
            current_offset += sizeof(int32_t);

            // Deserialize before_image_
            int32_t before_len = 0;
            memcpy(&before_len, data + current_offset, sizeof(int32_t));
            current_offset += sizeof(int32_t);
            if (before_len > 0)
            {
                rec.before_image_ = std::string(data + current_offset, before_len);
                current_offset += before_len;
            }

            // Deserialize after_image_
            int32_t after_len = 0;
            memcpy(&after_len, data + current_offset, sizeof(int32_t));
            current_offset += sizeof(int32_t);
            if (after_len > 0)
            {
                rec.after_image_ = std::string(data + current_offset, after_len);
                current_offset += after_len;
            }
        }

        if (rec.log_record_type_ == LogRecordType::CLR)
        {
            memcpy(&rec.undo_next_lsn_, data + current_offset, sizeof(int32_t));
            current_offset += sizeof(int32_t);
        }

        return rec;
    }
};
