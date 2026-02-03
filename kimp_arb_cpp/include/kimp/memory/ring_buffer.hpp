#pragma once

#include <atomic>
#include <array>
#include <optional>
#include <cstddef>
#include <new>
#include <type_traits>

namespace kimp::memory {

// Cache line size for padding
inline constexpr std::size_t CACHE_LINE_SIZE = 64;

/**
 * Lock-free Single-Producer Single-Consumer Ring Buffer
 *
 * Optimized for HFT with:
 * - Cache-line aligned head/tail to prevent false sharing
 * - Power-of-2 capacity for efficient modulo
 * - Memory ordering optimized for x86-64
 */
template<typename T, std::size_t Capacity>
class alignas(CACHE_LINE_SIZE) SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(Capacity > 0, "Capacity must be greater than 0");

private:
    static constexpr std::size_t MASK = Capacity - 1;

    // Data buffer
    alignas(CACHE_LINE_SIZE) std::array<T, Capacity> buffer_{};

    // Producer index (write position)
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> head_{0};

    // Consumer index (read position)
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> tail_{0};

    // Cached indices for reduced atomic loads
    alignas(CACHE_LINE_SIZE) std::size_t cached_tail_{0};  // For producer
    alignas(CACHE_LINE_SIZE) std::size_t cached_head_{0};  // For consumer

public:
    SPSCRingBuffer() = default;

    // Non-copyable, non-movable (due to atomics)
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&) = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&) = delete;

    /**
     * Try to push an item (producer only)
     * Returns true if successful, false if buffer is full
     */
    bool try_push(const T& item) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next_head = (head + 1) & MASK;

        // Check if buffer is full using cached tail
        if (next_head == cached_tail_) {
            // Reload tail from atomic
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next_head == cached_tail_) {
                return false;  // Buffer truly full
            }
        }

        buffer_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /**
     * Try to push an item with move semantics
     */
    bool try_push(T&& item) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next_head = (head + 1) & MASK;

        if (next_head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next_head == cached_tail_) {
                return false;
            }
        }

        buffer_[head] = std::move(item);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /**
     * Construct item in-place (producer only)
     */
    template<typename... Args>
    bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next_head = (head + 1) & MASK;

        if (next_head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next_head == cached_tail_) {
                return false;
            }
        }

        new (&buffer_[head]) T(std::forward<Args>(args)...);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /**
     * Try to pop an item (consumer only)
     * Returns std::nullopt if buffer is empty
     */
    std::optional<T> try_pop() noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);

        // Check if buffer is empty using cached head
        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) {
                return std::nullopt;  // Buffer truly empty
            }
        }

        T item = std::move(buffer_[tail]);
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return item;
    }

    /**
     * Pop into existing object (avoids move for large objects)
     */
    bool try_pop_into(T& item) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);

        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) {
                return false;
            }
        }

        item = std::move(buffer_[tail]);
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return true;
    }

    /**
     * Peek at front item without removing (consumer only)
     */
    const T* peek() const noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);

        if (tail == head) {
            return nullptr;
        }
        return &buffer_[tail];
    }

    /**
     * Check if buffer is empty
     */
    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    /**
     * Get current size (approximate, may be stale)
     */
    std::size_t size() const noexcept {
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & MASK;
    }

    /**
     * Get capacity
     */
    static constexpr std::size_t capacity() noexcept {
        return Capacity - 1;  // One slot reserved to distinguish full from empty
    }

    /**
     * Check if buffer is full
     */
    bool full() const noexcept {
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_acquire);
        return ((head + 1) & MASK) == tail;
    }
};

/**
 * Multi-Producer Multi-Consumer Ring Buffer using CAS
 * For cases where multiple threads need to push/pop
 */
template<typename T, std::size_t Capacity>
class alignas(CACHE_LINE_SIZE) MPMCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

private:
    static constexpr std::size_t MASK = Capacity - 1;

    struct Cell {
        std::atomic<std::size_t> sequence;
        T data;
    };

    alignas(CACHE_LINE_SIZE) std::array<Cell, Capacity> buffer_{};
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> dequeue_pos_{0};

public:
    MPMCRingBuffer() {
        for (std::size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MPMCRingBuffer(const MPMCRingBuffer&) = delete;
    MPMCRingBuffer& operator=(const MPMCRingBuffer&) = delete;

    bool try_push(const T& item) noexcept {
        Cell* cell;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & MASK];
            std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Buffer full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> try_pop() noexcept {
        Cell* cell;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & MASK];
            std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return std::nullopt;  // Buffer empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        T data = std::move(cell->data);
        cell->sequence.store(pos + MASK + 1, std::memory_order_release);
        return data;
    }

    bool empty() const noexcept {
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Cell* cell = &const_cast<MPMCRingBuffer*>(this)->buffer_[pos & MASK];
        std::size_t seq = cell->sequence.load(std::memory_order_acquire);
        return static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1) < 0;
    }

    static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }
};

} // namespace kimp::memory
