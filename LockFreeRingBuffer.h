#pragma once

#include <atomic>
#include <vector>
#include <cstdint>
#include <optional>
#include <cstddef>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define CPU_RELAX_RB() _mm_pause()
#else
#include <thread>
#define CPU_RELAX_RB() std::this_thread::yield()
#endif

/**
 * A simple, bounded, lock-free Multi-Producer Multi-Consumer (MPMC) Ring Buffer Queue.
 * Optimized for power-of-two capacities to use bitwise AND instead of modulo.
 */
template <typename T>
class LockFreeRingBuffer
{
private:
    struct alignas(64) Node
    {
        std::atomic<size_t> sequence;
        T data;
    };

    const size_t capacity_mask_;
    std::vector<Node> buffer_;

    alignas(64) std::atomic<size_t> enqueue_pos_{0};
    alignas(64) std::atomic<size_t> dequeue_pos_{0};

    // Helper to find the next power of two
    static size_t next_power_of_two(size_t v)
    {
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        v++;
        return v;
    }

public:
    explicit LockFreeRingBuffer(size_t capacity)
        : capacity_mask_(next_power_of_two(capacity) - 1),
          buffer_(next_power_of_two(capacity))
    {
        size_t cap = next_power_of_two(capacity);
        for (size_t i = 0; i < cap; ++i)
        {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool Push(T const &data)
    {
        Node *node;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;)
        {
            node = &buffer_[pos & capacity_mask_];
            size_t seq = node->sequence.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)pos;

            if (dif == 0)
            {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (dif < 0)
            {
                return false; // Queue is full
            }
            else
            {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
            CPU_RELAX_RB();
        }
        node->data = data;
        node->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool Pop(T &data)
    {
        Node *node;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;)
        {
            node = &buffer_[pos & capacity_mask_];
            size_t seq = node->sequence.load(std::memory_order_acquire);
            intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);

            if (dif == 0)
            {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (dif < 0)
            {
                return false; // Queue is empty
            }
            else
            {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
            CPU_RELAX_RB();
        }
        data = node->data;
        node->sequence.store(pos + capacity_mask_ + 1, std::memory_order_release);
        return true;
    }
};
