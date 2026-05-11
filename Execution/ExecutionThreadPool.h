#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <atomic>
#include <future>
#include <stdexcept>
#include <cstdlib> // For std::malloc and std::free

#if defined(__linux__)

#include <numa.h>
#include <pthread.h>
#endif

namespace Database
{
    /**
     * @brief A simple fixed-size thread pool to execute database morsel tasks.
     * With NUMA-aware allocation and affinity.
     */
    class ExecutionThreadPool
    {
    public:
        ExecutionThreadPool(size_t num_threads = std::thread::hardware_concurrency())
            : stop_(false)
        {
#if defined(__linux__)
            int num_nodes = numa_max_node() + 1;
#else
            int num_nodes = 1;
#endif

            for (size_t i = 0; i < num_threads; ++i)
            {
                workers_.emplace_back([this, i, num_nodes]
                                      {
#if defined(__linux__)
                    int numa_node = i % num_nodes;
                    struct bitmask* mask = numa_allocate_cpumask();
                    numa_node_to_cpus(numa_node, mask);
                    if (numa_sched_setaffinity(0, mask) != 0) {
                        // ignore error
                    }
                    numa_free_cpumask(mask);
#else
                    int numa_node = (i / 4) % num_nodes;
#endif

                    for (;;)
                    {
                        std::function<void(int)> task; // task now takes numa_node

                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex_);
                            this->condition_.wait(lock, [this] { return this->stop_ || !this->tasks_.empty(); });

                            if (this->stop_ && this->tasks_.empty())
                                return;

                            task = std::move(this->tasks_.front());
                            this->tasks_.pop();
                        }

                        task(numa_node);
                    } });
            }
        }

        ~ExecutionThreadPool()
        {
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                stop_ = true;
            }
            condition_.notify_all();
            for (std::thread &worker : workers_)
            {
                worker.join();
            }
        }

        template <class F, class... Args>
        auto Enqueue(F &&f, Args &&...args)
            -> std::future<typename std::invoke_result_t<F, int, Args...>>
        {
            using return_type = typename std::invoke_result_t<F, int, Args...>;

            auto task = std::make_shared<std::packaged_task<return_type(int)>>(
                std::bind(std::forward<F>(f), std::placeholders::_1, std::forward<Args>(args)...));

            std::future<return_type> res = task->get_future();
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                if (stop_)
                    throw std::runtime_error("enqueue on stopped ThreadPool");

                tasks_.emplace([task](int numa_node)
                               { (*task)(numa_node); });
            }
            condition_.notify_one();
            return res;
        }

        // Global singleton instance for the database
        static ExecutionThreadPool &GetInstance()
        {
            static ExecutionThreadPool instance;
            // hardware_concurrency
            return instance;
        }

    private:
        std::vector<std::thread> workers_;
        std::queue<std::function<void(int)>> tasks_;

        std::mutex queue_mutex_;
        std::condition_variable condition_;
        bool stop_;
    };

    inline void *NumaAllocLocal(size_t size)
    {
#if defined(__linux__)
        return numa_alloc_local(size);
#else
        return std::malloc(size);
#endif
    }

    inline void NumaFree(void *ptr, size_t size)
    {
#if defined(__linux__)
        if (ptr)
            numa_free(ptr, size);
#else
        std::free(ptr);
#endif
    }

} // namespace Database