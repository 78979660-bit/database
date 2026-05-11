#pragma once

#include "Transaction.h"
#include "../Storage/Tuple/Tuple.h"

namespace Database
{

    class Visibility
    {
    public:
        /**
         * 综合 MVCC 以及基于各种 Transaction 隔离级别的可见性判断逻辑。
         *
         * @param txn 当前尝试读取 Tuple 的事务
         * @param meta 该 Tuple 版本的控制元数据 (xmin, xmax)
         * @return 如果这行数据对该事务可见，则返回 true；否则返回 false
         */
        static bool IsVisibleTo(const Transaction *txn, const TupleMeta &meta)
        {
            txn_id_t current_txn_id = txn->GetTransactionId();
            IsolationLevel iso_level = txn->GetIsolationLevel();

            // 如果隔离级别是 READ_UNCOMMITTED (脏读)：只要没被逻辑或物理标记为真死，一切最新数据都给它看
            if (iso_level == IsolationLevel::READ_UNCOMMITTED)
            {
                if (meta.delete_txn_id_ == current_txn_id)
                    return false; // 自己的删除仍不可见
                return !meta.is_deleted_ && meta.delete_txn_id_ == INVALID_TXN_ID;
            }

            // ============================================
            // 1. 判断该版本的 "创建者" (insert_txn_id)
            // ============================================

            // 1.1 该版本是由“当前事务自己”产生的（且未曾完全回滚），那么可见，除非当前事务后来又删掉了它。
            if (meta.insert_txn_id_ == current_txn_id)
            {
                // 自己创建但又被自己删除了？如果被自己删除，就不可见。
                return meta.delete_txn_id_ != current_txn_id && !meta.is_deleted_;
            }

            // 1.2 该版本是由别的事务产生的
            //  - 如果是 READ_COMMITTED, 只要 insert_txn 尚未提交，我们就不可见(必须是已提交)。
            //  - 如果是 REPEATABLE_READ 或 SNAPSHOT_ISOLATION, 不仅不能是未提交的，还得是我们快照打点之前就完成的。
            bool insert_is_active = txn->IsTxnActive(meta.insert_txn_id_);
            bool insert_is_future = meta.insert_txn_id_ > current_txn_id;

            if (iso_level == IsolationLevel::READ_COMMITTED)
            {
                // 如果使用传统的 read-committed, "active" 判断通常需要根据当前系统的锁或直接查全局事务表
                // 在本架构锁管理保护下，它实际上能在解阻塞后读到。
                if (insert_is_active || meta.is_deleted_)
                {
                    return false;
                }
            }
            else
            {
                if (insert_is_active || insert_is_future || meta.is_deleted_)
                {
                    return false; // 创建动作未提交或发生在我们打快照之后
                }
            }

            // ============================================
            // 2. 根据到这里的代码流推断，到达这里的 Tuple 肯定是在我们开始前就已经`提交(Committed)`并产生的版本。
            // 现在我们要判断这个可见的版本，后来是否被合法地"删除" 或 "修改(本质是删旧写新)" 了？
            // ============================================

            if (meta.delete_txn_id_ != INVALID_TXN_ID)
            {
                // 2.1 该行被我们当前事务自己删除了
                if (meta.delete_txn_id_ == current_txn_id)
                {
                    return false;
                }

                // 2.2 该行被别人删除了。
                // 如果是 READ_COMMITTED：只要别人的删除还没提交，我们就继续读老版本(即认为未被删)。
                //      - 且由于 Exec 时有 LockManager SharedLock 保护，ReadCommitted 只有在写者结束释放排他锁后才会读。
                // 如果是 REPEATABLE_READ / SNAPSHOT / SERIALIZABLE：只要别人的删除不处于我们可见的过去(快照打底)，该删除统统忽视，行仍可见。

                bool delete_is_active = txn->IsTxnActive(meta.delete_txn_id_);
                bool delete_is_future = meta.delete_txn_id_ > current_txn_id;

                if (iso_level == IsolationLevel::READ_COMMITTED)
                {
                    if (delete_is_active)
                        return true; // 删除未提交对RC也是暂不可见的
                }
                else
                {
                    if (delete_is_active || delete_is_future)
                    {
                        return true;
                    }
                }

                // 删除动作是在我们可见范围内提交的，所以行真的不存在了。
                return false;
            }

            // 没有人执行过删除/修改，当然对我们就是可见的
            return true;
        }
    };

} // namespace Database
