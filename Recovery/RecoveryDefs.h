#pragma once

#include <cstdint>

// Alias for Log Sequence Number
using lsn_t = int32_t;
using txn_id_t = int32_t;
using page_id_t = int32_t;

// Invalid LSN constant
static constexpr lsn_t INVALID_LSN = -1;
static constexpr txn_id_t INVALID_TXN_ID = -1;

// Size of Log Header (LSN + Size + ...)
static constexpr int LOG_HEADER_SIZE = 20; // 4 (size) + 4 (LSN) + 4 (TransID) + 4 (PrevLSN) + 4 (LogType)

enum class LogRecordType
{
    INVALID = 0,
    INSERT,
    MARK_TOMBSTONE,
    APPLY_DELETE,
    ROLLBACK_DELETE,
    UPDATE,
    BEGIN,
    COMMIT,
    ABORT,
    NEW_PAGE, // Allocation of a new page
    CHECKPOINT_BEGIN,
    CHECKPOINT_END,
    // Compensation Log Record (CLR) to avoid undoing an undo
    CLR
};
