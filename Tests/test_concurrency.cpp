#include <iostream>
#include <vector>
#include <cassert>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <random>
#include "../Concurrency/LockManager.h"
#include "../Concurrency/Transaction.h"

using namespace Database;

// Helper to assert conditions
#define ASSERT_TRUE(cond)                                                                                 \
    if (!(cond))                                                                                          \
    {                                                                                                     \
        std::cerr << "Assertion failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        exit(1);                                                                                          \
    }

void test_basic_locking()
{
    std::cout << "Testing basic locking..." << std::endl;
    LockManager lock_mgr;
    Transaction txn1(1, IsolationLevel::READ_COMMITTED);
    RID rid1(1, 0); // page_id = 1, slot_num = 0
    RID rid2(2, 0);

    // Test Shared Lock
    ASSERT_TRUE(lock_mgr.LockShared(&txn1, rid1));
    // Test Exclusive Lock
    ASSERT_TRUE(lock_mgr.LockExclusive(&txn1, rid2));

    // Release Locks
    ASSERT_TRUE(lock_mgr.Unlock(&txn1, rid1));
    ASSERT_TRUE(lock_mgr.Unlock(&txn1, rid2));
    std::cout << "Basic locking test passed.\n";
}

void test_deadlock_detection()
{
    std::cout << "Testing deadlock detection graph..." << std::endl;
    LockManager lock_mgr;

    // T1 waits for T2
    lock_mgr.AddEdge(1, 2);
    // T2 waits for T3
    lock_mgr.AddEdge(2, 3);

    txn_id_t cycle_txn = -1;
    ASSERT_TRUE(!lock_mgr.HasCycle(cycle_txn)); // No cycle yet

    // T3 waits for T1 (Cycle created)
    lock_mgr.AddEdge(3, 1);

    ASSERT_TRUE(lock_mgr.HasCycle(cycle_txn));
    std::cout << "Deadlock detected involving txn: " << cycle_txn << std::endl;

    // Breaking the cycle
    lock_mgr.RemoveEdge(3, 1);
    ASSERT_TRUE(!lock_mgr.HasCycle(cycle_txn));

    std::cout << "Deadlock detection test passed.\n";
}

void test_high_concurrency()
{
    std::cout << "Testing high concurrency locking..." << std::endl;
    LockManager lock_mgr;
    const int NUM_THREADS = 50;
    const int NUM_RIDS = 1000;
    const int NUM_OPS = 5;

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> abort_count{0};

    auto worker = [&](int tid)
    {
        Transaction txn(tid, IsolationLevel::REPEATABLE_READ);

        std::random_device rd;
        std::mt19937 gen(rd() + tid);
        std::uniform_int_distribution<> rid_dist(0, NUM_RIDS - 1);
        std::uniform_int_distribution<> sleep_dist(10, 200);
        std::uniform_int_distribution<> lock_type_dist(0, 100);

        std::vector<std::pair<RID, bool>> acquired_locks;

        // Growing Phase
        for (int i = 0; i < NUM_OPS; ++i)
        {
            RID rid(1, rid_dist(gen));
            bool is_shared = (lock_type_dist(gen) < 70);
            bool locked = false;

            if (is_shared)
            {
                locked = lock_mgr.LockShared(&txn, rid);
            }
            else
            {
                locked = lock_mgr.LockExclusive(&txn, rid);
            }

            if (!locked || txn.GetState() == TransactionState::ABORTED)
            {
                abort_count++;
                // Release all acquired locks on abort
                for (auto &l : acquired_locks)
                {
                    lock_mgr.Unlock(&txn, l.first);
                }
                return;
            }

            acquired_locks.push_back({rid, is_shared});

            // Simulate realistic transaction query work mapping
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_dist(gen)));
        }

        // Shrinking Phase (Commit)
        for (auto &l : acquired_locks)
        {
            lock_mgr.Unlock(&txn, l.first);
        }

        success_count++;
    };

    for (int i = 1; i <= NUM_THREADS; ++i)
    {
        threads.emplace_back(worker, i);
    }

    for (auto &t : threads)
    {
        t.join();
    }

    std::cout << "Successful Txns: " << success_count << " Aborted Txns: " << abort_count << std::endl;
    std::cout << "High concurrency test finished.\n";
}

int main()
{
    std::cout << "Starting Concurrency Tests...\n";
    test_basic_locking();
    test_deadlock_detection();
    test_high_concurrency();
    std::cout << "All Concurrency Tests Passed!\n";
    return 0;
}