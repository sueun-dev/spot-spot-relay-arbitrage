#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <atomic>
#include <new>
#include <cassert>

namespace kimp::memory {

/**
 * Thread-safe Object Pool with pre-allocated memory
 *
 * Features:
 * - Lock-free allocation/deallocation using atomic free list
 * - Pre-allocated at construction time (no allocation during operation)
 * - Cache-line aligned for performance
 */
template<typename T, std::size_t PoolSize = 1024>
class ObjectPool {
    static_assert(PoolSize > 0, "Pool size must be greater than 0");

private:
    struct Node {
        alignas(alignof(T)) std::array<std::byte, sizeof(T)> storage;
        std::atomic<Node*> next{nullptr};
    };

    alignas(64) std::array<Node, PoolSize> pool_{};
    alignas(64) std::atomic<Node*> free_list_{nullptr};
    alignas(64) std::atomic<std::size_t> allocated_count_{0};

public:
    ObjectPool() {
        // Initialize free list (reverse order so first allocation gets pool_[0])
        for (std::size_t i = PoolSize; i > 0; --i) {
            Node* node = &pool_[i - 1];
            node->next.store(free_list_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
            free_list_.store(node, std::memory_order_relaxed);
        }
    }

    ~ObjectPool() {
        // Destructor: objects should already be deallocated
        // In debug mode, assert all objects returned
        assert(allocated_count_.load() == 0 && "Objects still allocated on pool destruction");
    }

    // Non-copyable, non-movable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /**
     * Allocate and construct an object
     * Returns nullptr if pool is exhausted
     */
    template<typename... Args>
    T* allocate(Args&&... args) {
        Node* node = pop_free_list();
        if (!node) {
            return nullptr;  // Pool exhausted
        }

        allocated_count_.fetch_add(1, std::memory_order_relaxed);

        // Construct object in-place
        return new (node->storage.data()) T(std::forward<Args>(args)...);
    }

    /**
     * Destroy and deallocate an object
     */
    void deallocate(T* ptr) noexcept {
        if (!ptr) return;

        // Call destructor
        ptr->~T();

        // Return to free list
        Node* node = reinterpret_cast<Node*>(
            reinterpret_cast<std::byte*>(ptr) - offsetof(Node, storage));

        push_free_list(node);
        allocated_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * Get number of currently allocated objects
     */
    std::size_t allocated() const noexcept {
        return allocated_count_.load(std::memory_order_relaxed);
    }

    /**
     * Get number of available objects
     */
    std::size_t available() const noexcept {
        return PoolSize - allocated();
    }

    /**
     * Get pool capacity
     */
    static constexpr std::size_t capacity() noexcept {
        return PoolSize;
    }

    /**
     * Check if pool is exhausted
     */
    bool empty() const noexcept {
        return allocated() >= PoolSize;
    }

private:
    Node* pop_free_list() noexcept {
        Node* head = free_list_.load(std::memory_order_acquire);
        while (head) {
            Node* next = head->next.load(std::memory_order_relaxed);
            if (free_list_.compare_exchange_weak(head, next,
                    std::memory_order_release, std::memory_order_acquire)) {
                return head;
            }
        }
        return nullptr;
    }

    void push_free_list(Node* node) noexcept {
        Node* head = free_list_.load(std::memory_order_relaxed);
        do {
            node->next.store(head, std::memory_order_relaxed);
        } while (!free_list_.compare_exchange_weak(head, node,
                    std::memory_order_release, std::memory_order_relaxed));
    }
};

/**
 * RAII wrapper for pooled objects
 */
template<typename T, std::size_t PoolSize = 1024>
class PooledPtr {
private:
    T* ptr_{nullptr};
    ObjectPool<T, PoolSize>* pool_{nullptr};

public:
    PooledPtr() = default;

    PooledPtr(T* ptr, ObjectPool<T, PoolSize>* pool)
        : ptr_(ptr), pool_(pool) {}

    ~PooledPtr() {
        if (ptr_ && pool_) {
            pool_->deallocate(ptr_);
        }
    }

    // Move only
    PooledPtr(const PooledPtr&) = delete;
    PooledPtr& operator=(const PooledPtr&) = delete;

    PooledPtr(PooledPtr&& other) noexcept
        : ptr_(other.ptr_), pool_(other.pool_) {
        other.ptr_ = nullptr;
        other.pool_ = nullptr;
    }

    PooledPtr& operator=(PooledPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_ && pool_) {
                pool_->deallocate(ptr_);
            }
            ptr_ = other.ptr_;
            pool_ = other.pool_;
            other.ptr_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }

    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T* release() noexcept {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

    void reset() {
        if (ptr_ && pool_) {
            pool_->deallocate(ptr_);
        }
        ptr_ = nullptr;
    }
};

/**
 * Message Buffer Pool for WebSocket messages
 */
class MessageBufferPool {
public:
    static constexpr std::size_t BUFFER_SIZE = 4096;
    static constexpr std::size_t POOL_SIZE = 256;

    struct Buffer {
        std::array<char, BUFFER_SIZE> data{};
        std::size_t size{0};
        std::atomic<Buffer*> next{nullptr};

        char* begin() noexcept { return data.data(); }
        char* end() noexcept { return data.data() + size; }
        const char* begin() const noexcept { return data.data(); }
        const char* end() const noexcept { return data.data() + size; }

        void clear() noexcept { size = 0; }

        std::string_view view() const noexcept {
            return std::string_view(data.data(), size);
        }
    };

private:
    std::array<Buffer, POOL_SIZE> pool_{};
    std::atomic<Buffer*> free_list_{nullptr};

public:
    MessageBufferPool() {
        for (std::size_t i = POOL_SIZE; i > 0; --i) {
            Buffer* buf = &pool_[i - 1];
            buf->next.store(free_list_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
            free_list_.store(buf, std::memory_order_relaxed);
        }
    }

    Buffer* acquire() noexcept {
        Buffer* head = free_list_.load(std::memory_order_acquire);
        while (head) {
            Buffer* next = head->next.load(std::memory_order_relaxed);
            if (free_list_.compare_exchange_weak(head, next,
                    std::memory_order_release, std::memory_order_acquire)) {
                head->clear();
                return head;
            }
        }
        return nullptr;  // Pool exhausted
    }

    void release(Buffer* buffer) noexcept {
        if (!buffer) return;

        Buffer* head = free_list_.load(std::memory_order_relaxed);
        do {
            buffer->next.store(head, std::memory_order_relaxed);
        } while (!free_list_.compare_exchange_weak(head, buffer,
                    std::memory_order_release, std::memory_order_relaxed));
    }
};

/**
 * RAII wrapper for message buffers
 */
class MessageBufferGuard {
private:
    MessageBufferPool::Buffer* buffer_{nullptr};
    MessageBufferPool* pool_{nullptr};

public:
    MessageBufferGuard(MessageBufferPool& pool)
        : buffer_(pool.acquire()), pool_(&pool) {}

    ~MessageBufferGuard() {
        if (buffer_ && pool_) {
            pool_->release(buffer_);
        }
    }

    MessageBufferGuard(const MessageBufferGuard&) = delete;
    MessageBufferGuard& operator=(const MessageBufferGuard&) = delete;

    MessageBufferGuard(MessageBufferGuard&& other) noexcept
        : buffer_(other.buffer_), pool_(other.pool_) {
        other.buffer_ = nullptr;
        other.pool_ = nullptr;
    }

    MessageBufferPool::Buffer* get() const noexcept { return buffer_; }
    MessageBufferPool::Buffer* operator->() const noexcept { return buffer_; }

    explicit operator bool() const noexcept { return buffer_ != nullptr; }
};

} // namespace kimp::memory
