#pragma once

#include <atomic>
#include <vector>
#include <memory>
#include <thread>
#include <cstdint>
#include <iostream>
#include "../Recovery/LogManager.h"

namespace Database
{

    // Logical Page ID for indirection
    using PID = uint64_t;

    // Forward declarations
    template <typename KeyType, typename ValueType, typename KeyComparator>
    class BwNode;

    const PID INVALID_PID = static_cast<PID>(-1);

    // Concept: The Mapping Table maps logical PIDs to physical memory pointers.
    // This allows atomic updates via CAS on the mapping table entry.
    template <typename KeyType, typename ValueType, typename KeyComparator>
    class MappingTable
    {
    public:
        MappingTable(size_t initial_size = 1000000) : table_(initial_size)
        {
            for (auto &entry : table_)
            {
                entry.store(nullptr, std::memory_order_relaxed);
            }
        }

        // Get the physical pointer for a given PID
        BwNode<KeyType, ValueType, KeyComparator> *Get(PID pid) const
        {
            return table_[pid].load(std::memory_order_acquire);
        }

        // Atomically swap the mapping from old_node to new_node
        bool CAS(PID pid, BwNode<KeyType, ValueType, KeyComparator> *old_node, BwNode<KeyType, ValueType, KeyComparator> *new_node)
        {
            return table_[pid].compare_exchange_strong(old_node, new_node, std::memory_order_acq_rel);
        }

        // Allocate a new PID
        PID AllocatePID()
        {
            return next_pid_.fetch_add(1, std::memory_order_relaxed);
        }

    private:
        std::vector<std::atomic<BwNode<KeyType, ValueType, KeyComparator> *>> table_;
        std::atomic<PID> next_pid_{0};
    };

    // Base class for all nodes in the BwTree (Base nodes and Delta nodes)
    template <typename KeyType, typename ValueType, typename KeyComparator>
    class BwNode
    {
    public:
        enum class NodeType
        {
            BasePage,
            InsertDelta,
            DeleteDelta,
            UpdateDelta,
            SplitDelta,
            RemoveNodeDelta
        };

        virtual ~BwNode() = default;
        virtual NodeType GetType() const = 0;

        lsn_t lsn_ = INVALID_LSN;
    };

    // Delta node for insert operations
    template <typename KeyType, typename ValueType, typename KeyComparator>
    class InsertDeltaNode : public BwNode<KeyType, ValueType, KeyComparator>
    {
    public:
        InsertDeltaNode(const KeyType &key, const ValueType &value, BwNode<KeyType, ValueType, KeyComparator> *next)
            : key_(key), value_(value), next_(next) {}

        typename BwNode<KeyType, ValueType, KeyComparator>::NodeType GetType() const override
        {
            return BwNode<KeyType, ValueType, KeyComparator>::NodeType::InsertDelta;
        }

        KeyType key_;
        ValueType value_;
        BwNode<KeyType, ValueType, KeyComparator> *next_; // Pointer to the previous state of the page
    };

    // Delta node for split operations
    template <typename KeyType, typename ValueType, typename KeyComparator>
    class SplitDeltaNode : public BwNode<KeyType, ValueType, KeyComparator>
    {
    public:
        SplitDeltaNode(const KeyType &split_key, PID split_node_pid, BwNode<KeyType, ValueType, KeyComparator> *next)
            : split_key_(split_key), split_node_pid_(split_node_pid), next_(next) {}

        typename BwNode<KeyType, ValueType, KeyComparator>::NodeType GetType() const override
        {
            return BwNode<KeyType, ValueType, KeyComparator>::NodeType::SplitDelta;
        }

        KeyType split_key_;  // The key at which the split happened
        PID split_node_pid_; // The PID of the newly created right sibling node
        BwNode<KeyType, ValueType, KeyComparator> *next_;
    };

    // Base Page representing a materialized node (leaf or internal)
    template <typename KeyType, typename ValueType, typename KeyComparator>
    class BasePageNode : public BwNode<KeyType, ValueType, KeyComparator>
    {
    public:
        BasePageNode() : right_sibling_(INVALID_PID), has_high_key_(false) {} // conventionally invalid PID

        typename BwNode<KeyType, ValueType, KeyComparator>::NodeType GetType() const override
        {
            return BwNode<KeyType, ValueType, KeyComparator>::NodeType::BasePage;
        }

        // Actual index data would be stored here (e.g., sorted vector of key-value pairs)
        std::vector<std::pair<KeyType, ValueType>> data_;
        PID right_sibling_;
        bool has_high_key_;
        KeyType high_key_; // Used in B-link tree traversal to know if we must follow right_sibling_
    };

    // The Epoch Manager handles garbage collection of old nodes
    class EpochManager
    {
    public:
        void EnterEpoch()
        {
            // Register thread in current epoch
        }

        void LeaveEpoch()
        {
            // Unregister thread
        }

        void MarkNodeForReclamation(void *node)
        {
            // Add node to current epoch's garbage list
        }

        void PerformGC()
        {
            // Reclaim nodes from safe epochs
        }
    };

    // The BwTree implementation
    template <typename KeyType, typename ValueType, typename KeyComparator>
    class BwTree
    {
    public:
        BwTree(LogManager *log_manager = nullptr) : mapping_table_(), log_manager_(log_manager)
        {
            // Initialize an empty root node
            root_pid_ = mapping_table_.AllocatePID();
            auto initial_root = new BasePageNode<KeyType, ValueType, KeyComparator>();
            mapping_table_.CAS(root_pid_, nullptr, initial_root);
        }

        ~BwTree()
        {
            // Cleanup would normally go through epoch manager
        }

        bool Insert(const KeyType &key, const ValueType &value, txn_id_t txn_id = 1)
        {
            epoch_manager_.EnterEpoch();

            PID target_pid = root_pid_;
            KeyComparator comparator;

            while (true)
            {
                // First, find the correct PID by following B-Link pointers.
                // Re-evaluate target_pid if it has split (similar to Get traversal)
                BwNode<KeyType, ValueType, KeyComparator> *current_head = mapping_table_.Get(target_pid);
                BwNode<KeyType, ValueType, KeyComparator> *current_node = current_head;
                bool moved_right = false;

                while (current_node != nullptr)
                {
                    if (current_node->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::SplitDelta)
                    {
                        auto split_node = static_cast<SplitDeltaNode<KeyType, ValueType, KeyComparator> *>(current_node);
                        if (!comparator(key, split_node->split_key_))
                        {
                            target_pid = split_node->split_node_pid_;
                            moved_right = true;
                            break;
                        }
                        current_node = split_node->next_;
                    }
                    else if (current_node->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::BasePage)
                    {
                        auto base_page = static_cast<BasePageNode<KeyType, ValueType, KeyComparator> *>(current_node);
                        if (base_page->has_high_key_ && !comparator(key, base_page->high_key_))
                        {
                            target_pid = base_page->right_sibling_;
                            moved_right = true;
                            break;
                        }
                        break;
                    }
                    else if (current_node->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::InsertDelta)
                    {
                        current_node = static_cast<InsertDeltaNode<KeyType, ValueType, KeyComparator> *>(current_node)->next_;
                    }
                    else
                    {
                        break;
                    }
                }

                if (moved_right)
                {
                    // Retry with the new target_pid
                    continue;
                }

                // If we didn't move right, we are at the right node.
                // We must now re-read the head of the mapped PID and try to CAS.
                current_head = mapping_table_.Get(target_pid);

                // Create a new delta node pointing to the current head
                auto delta_node = new InsertDeltaNode<KeyType, ValueType, KeyComparator>(key, value, current_head);

                // Attempt to CAS the mapping table
                if (mapping_table_.CAS(target_pid, current_head, delta_node))
                {
                    // Success!
                    if (log_manager_)
                    {
                        // Physiological Logging: Record the logical operation
                        int32_t pid_as_int = static_cast<int32_t>(target_pid);
                        LogRecord log_record(txn_id, INVALID_LSN, LogRecordType::INSERT, pid_as_int, 0);
                        // log_record.after_image_ could be populated with key/value in a complete implementation
                        lsn_t assigned_lsn = log_manager_->AppendLogRecord(&log_record);
                        delta_node->lsn_ = assigned_lsn;
                    }
                    epoch_manager_.LeaveEpoch();
                    return true;
                }

                // CAS failed (another thread updated the page), retry
                delete delta_node;
            }
        }

        bool Get(const KeyType &key, ValueType *result)
        {
            epoch_manager_.EnterEpoch();

            PID current_pid = root_pid_;
            KeyComparator comparator;

            while (current_pid != INVALID_PID)
            {
                BwNode<KeyType, ValueType, KeyComparator> *current_node = mapping_table_.Get(current_pid);
                bool moved_right = false;

                // Traverse the delta chain and base page for the current PID
                while (current_node != nullptr)
                {
                    if (current_node->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::SplitDelta)
                    {
                        auto split_node = static_cast<SplitDeltaNode<KeyType, ValueType, KeyComparator> *>(current_node);
                        // B-Link Tree logic: if target key >= split_key, the value has moved to the right sibling.
                        if (!comparator(key, split_node->split_key_)) // key >= split_key
                        {
                            current_pid = split_node->split_node_pid_;
                            moved_right = true;
                            break; // Stop searching this PID, restart while loop with new PID
                        }
                        current_node = split_node->next_;
                    }
                    else if (current_node->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::InsertDelta)
                    {
                        auto insert_delta = static_cast<InsertDeltaNode<KeyType, ValueType, KeyComparator> *>(current_node);
                        if (!comparator(insert_delta->key_, key) && !comparator(key, insert_delta->key_)) // key == insert_delta->key_
                        {
                            // Match found in delta chain!
                            *result = insert_delta->value_;
                            epoch_manager_.LeaveEpoch(); // std::cout removed
                            return true;
                        }

                        current_node = insert_delta->next_;
                    }
                    else if (current_node->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::BasePage)
                    {
                        auto base_page = static_cast<BasePageNode<KeyType, ValueType, KeyComparator> *>(current_node);
                        // B-Link Check the high key of the base page
                        if (base_page->has_high_key_ && !comparator(key, base_page->high_key_))
                        {
                            current_pid = base_page->right_sibling_;
                            moved_right = true;
                            break;
                        }

                        // Normal search within the base page
                        for (const auto &pair : base_page->data_)
                        {
                            if (!comparator(pair.first, key) && !comparator(key, pair.first))
                            {
                                *result = pair.second;
                                epoch_manager_.LeaveEpoch();
                                return true;
                            }
                        }
                        break; // End of chain and not found
                    }
                    else
                    {
                        // Handle deletion deltas, update deltas, etc.
                        break;
                    }
                }

                if (!moved_right)
                {
                    // In a full tree, we would check if this is an internal node and follow the child pointer.
                    // For now, if we didn't move right, and we searched the node, we conclude it's not here.
                    break;
                }
            }

            epoch_manager_.LeaveEpoch();
            return false;
        }

        // Periodically consolidate delta chains into new base pages to maintain search speed
        void Consolidate(PID pid)
        {
            epoch_manager_.EnterEpoch();

            while (true)
            {
                BwNode<KeyType, ValueType, KeyComparator> *current_head = mapping_table_.Get(pid);

                // If it's already a base page, no need to consolidate
                if (!current_head || current_head->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::BasePage)
                {
                    break;
                }

                // Create a completely new base page
                auto new_base = new BasePageNode<KeyType, ValueType, KeyComparator>();

                // Collect the state
                BwNode<KeyType, ValueType, KeyComparator> *iter = current_head;
                BasePageNode<KeyType, ValueType, KeyComparator> *old_base = nullptr;

                // This list collects delta writes backwards.
                std::vector<std::pair<KeyType, ValueType>> delta_inserts;

                while (iter)
                {
                    if (iter->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::BasePage)
                    {
                        old_base = static_cast<BasePageNode<KeyType, ValueType, KeyComparator> *>(iter);
                        break;
                    }
                    else if (iter->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::InsertDelta)
                    {
                        auto insert_node = static_cast<InsertDeltaNode<KeyType, ValueType, KeyComparator> *>(iter);
                        delta_inserts.push_back({insert_node->key_, insert_node->value_});
                        iter = insert_node->next_;
                    }
                    else if (iter->GetType() == BwNode<KeyType, ValueType, KeyComparator>::NodeType::SplitDelta)
                    {
                        auto split_node = static_cast<SplitDeltaNode<KeyType, ValueType, KeyComparator> *>(iter);
                        // A split implies that keys greater than or equal to split_node->split_key_ were moved to another node.
                        if (!new_base->has_high_key_)
                        {
                            new_base->right_sibling_ = split_node->split_node_pid_;
                            new_base->high_key_ = split_node->split_key_;
                            new_base->has_high_key_ = true;
                        }
                        iter = split_node->next_;
                    }
                    else
                    {
                        // Handle other types of deltas...
                        iter = nullptr;
                    }
                }

                // Build new state
                if (old_base)
                {
                    // For a proper BwTree, you'd merge sort delta_inserts (which are unordered relative to base page)
                    // with the sorted old_base->data_ array. Here we do an illustrative copy.
                    new_base->data_ = old_base->data_;
                    if (new_base->right_sibling_ == INVALID_PID && old_base->right_sibling_ != INVALID_PID)
                    {
                        new_base->right_sibling_ = old_base->right_sibling_;
                    }
                }

                for (const auto &kv : delta_inserts)
                {
                    new_base->data_.push_back(kv);
                }

                // Swap in mapping table
                if (mapping_table_.CAS(pid, current_head, new_base))
                {
                    // Wait for an epoch before deleting the old chain
                    epoch_manager_.MarkNodeForReclamation(current_head);
                    break;
                }
                else
                {
                    delete new_base; // CAS failed, someone else appended a delta. Retry.
                }
            }
            epoch_manager_.LeaveEpoch();
        }

    private:
        MappingTable<KeyType, ValueType, KeyComparator> mapping_table_;
        PID root_pid_;
        EpochManager epoch_manager_;
        LogManager *log_manager_;
    };

} // namespace Database
