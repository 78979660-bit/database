#pragma once

#include <memory>
#include "../BufferPoolManager.h"
#include "../Catalog/Catalog.h"
#include "../Concurrency/LockManager.h"

namespace Database
{

    class Transaction; // Forward declaration

    class ExecutorContext
    {
    public:
        ExecutorContext(Catalog *catalog, BufferPoolManager *bpm, Transaction *txn = nullptr, LockManager *lock_manager = nullptr)
            : catalog_(catalog), bpm_(bpm), txn_(txn), lock_manager_(lock_manager) {}

        Catalog *GetCatalog() { return catalog_; }
        BufferPoolManager *GetBufferPoolManager() { return bpm_; }
        Transaction *GetTransaction() { return txn_; }
        LockManager *GetLockManager() { return lock_manager_; }

    private:
        Catalog *catalog_;
        BufferPoolManager *bpm_;
        Transaction *txn_;
        LockManager *lock_manager_;
    };

} // namespace Database