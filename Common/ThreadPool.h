#pragma once

#include <vector>

#include <thread>

#include <future>

#include <atomic>

#include <functional>

#include <memory>

#include <immintrin.h>

namespace Database

{

    // [极致无锁形队列] MPMC (多生产?多消费? 结构，硬核消除一切系统调?

    template <typename T, size_t Capacity>

    class LockFreeQueue
    {

        struct alignas(64) Item
        {

            std::atomic<size_t> seq;

            T data;
        };

        std::vector<Item> buffer_;

        alignas(64) std::atomic<size_t> head_{0};

        alignas(64) std::atomic<size_t> tail_{0};

    public:
        LockFreeQueue() : buffer_(Capacity)
        {

            for (size_t i = 0; i < Capacity; ++i)
            {

                buffer_[i].seq.store(i, std::memory_order_relaxed);
            }
        }

        bool Enqueue(T const &data)
        {

            Item *cell;

            size_t pos = tail_.load(std::memory_order_relaxed);

            for (;;)
            {

                cell = &buffer_[pos % Capacity];

                size_t seq = cell->seq.load(std::memory_order_acquire);

                intptr_t dif = (intptr_t)seq - (intptr_t)pos;

                if (dif == 0)
                {

                    if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {

                        break;
                    }
                }
                else if (dif < 0)
                {

                    _mm_pause();  // 让出 CPU 流水线
                    return false; // Full
                }
                else
                {

                    pos = tail_.load(std::memory_order_relaxed);
                }

                _mm_pause(); // 改进线程退让策略：插入硬件退让指令降低内存总线压力
            }

            cell->data = data;

            cell->seq.store(pos + 1, std::memory_order_release);

            return true;
        }

        bool Dequeue(T &data)
        {

            Item *cell;

            size_t pos = head_.load(std::memory_order_relaxed);

            for (;;)
            {

                cell = &buffer_[pos % Capacity];

                size_t seq = cell->seq.load(std::memory_order_acquire);

                intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);

                if (dif == 0)
                {

                    if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {

                        break;
                    }
                }
                else if (dif < 0)
                {

                    _mm_pause();  // 让出CPU流水线资源
                    return false; // Empty
                }
                else
                {

                    pos = head_.load(std::memory_order_relaxed);
                }

                _mm_pause(); // 改进线程退让策略：空转与冲突时缓解 MESI 风暴
            }

            data = std::move(cell->data);

            cell->seq.store(pos + Capacity, std::memory_order_release);

            return true;
        }
    };

    // [超微秒级] 无锁工作窃取静线程池 (Work-Stealing Spin-Wait ThreadPool)

    class ThreadPool

    {

    public:
        static ThreadPool &Instance()
        {

            static ThreadPool instance(std::thread::hardware_concurrency());

            return instance;
        }

        template <class F, class... Args>

        auto Enqueue(F &&f, Args &&...args) -> std::future<typename std::invoke_result_t<F, Args...>>
        {

            using return_type = typename std::invoke_result_t<F, Args...>;

            auto task = std::make_shared<std::packaged_task<return_type()>>(

                std::bind(std::forward<F>(f), std::forward<Args>(args)...)

            );

            std::future<return_type> res = task->get_future();

            std::function<void()> wrapper_task = [task]()
            { (*task)(); };

            // 极派发：询投入应线程的无锁队列中

            size_t target = next_queue_.fetch_add(1, std::memory_order_relaxed) % queues_.size();

            while (!queues_[target]->Enqueue(wrapper_task))
            {

                target = (target + 1) % queues_.size();
            }

            return res;
        }

        ~ThreadPool()
        {

            stop_.store(true, std::memory_order_release);

            for (std::thread &worker : workers_)
            {

                if (worker.joinable())
                    worker.join();
            }
        }

        size_t GetThreadCount() const { return workers_.size(); }

    private:
        ThreadPool(size_t threads) : stop_(false), next_queue_(0)
        {

            if (threads == 0)
                threads = 4;

            for (size_t i = 0; i < threads; ++i)
            {

                queues_.emplace_back(std::make_unique<LockFreeQueue<std::function<void()>, 1024>>());
            }

            for (size_t i = 0; i < threads; ++i)
            {

                workers_.emplace_back([this, i]
                                      {

                    std::function<void()> local_batch[4]; // [优化] 微批次上限调为 4，防止长耗时大任务被单个核心独吞导致负载不均
                    uint32_t spin_count = 0;

                    

                    while (true) {

                        size_t batch_size = 0;

                        

                        // 1. 本地无锁队列批量提取 (Micro-Batch Dequeue)

                        while (batch_size < 2 && this->queues_[i]->Dequeue(local_batch[batch_size])) {

                            batch_size++;

                        }

                        if (batch_size == 0) {

                            // 2. 工作窃取 (Work-stealing)：限制窃取范围，降低微架构的分支惩罚与内存总线冲突
                            size_t max_steal = std::min(this->queues_.size(), (size_t)8);
                            for (size_t offset = 1; offset < max_steal; ++offset) {

                                size_t target = (i + offset) % this->queues_.size();

                                // 窃取别人的一小半，避免窃取过多导致原核心饿死
                                while (batch_size < 3 && this->queues_[target]->Dequeue(local_batch[batch_size])) {

                                    batch_size++;

                                }

                                if (batch_size > 0) break;

                            }

                        }



                        if (batch_size > 0) {

                            for (size_t k = 0; k < batch_size; ++k) {
                                local_batch[k]();
                                local_batch[k] = nullptr; // 释放绑定的指针资源
                            }

                            spin_count = 0; // 重置旋数?

                        } else {

                            if (this->stop_.load(std::memory_order_acquire)) {

                                break;

                            }

                            // 3. (Spin-wait) + 退让(Yield) 淘汰互斥锁，更陡峭的退阶策略
                            // 避免大量死转引发的 CPI 恶化，加速空闲线程让出执行端口

                            ++spin_count;
                            if (spin_count < 200) {
                                _mm_pause();
                            } else if (spin_count < 10000) {
                                std::this_thread::yield();
                            } else {
                                std::this_thread::sleep_for(std::chrono::microseconds(100)); // Sleep on E-cores to drop Back-End Bound / Serializing Ops
                            }
                        }
                    } });
            }
        }

        std::vector<std::unique_ptr<LockFreeQueue<std::function<void()>, 1024>>> queues_;

        std::vector<std::thread> workers_;

        std::atomic<size_t> next_queue_;

        alignas(64) std::atomic<bool> stop_;
    };

}
