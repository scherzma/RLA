#pragma once

#include <atomic>
#include <cstddef>
#include <array>
#include <optional>

namespace RLA {

// Lock-free Single-Producer Single-Consumer ring buffer
// Size must be a power of 2 for efficient modulo operations
template<typename T, size_t Size>
class RingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");
    static_assert(Size > 0, "Size must be greater than 0");

public:
    RingBuffer() : head_(0), tail_(0) {
        buffer_.fill(T{});
    }

    // Returns true if push succeeded, false if buffer is full
    bool push(const T& item) noexcept {
        const size_t currentHead = head_.load(std::memory_order_relaxed);
        const size_t nextHead = (currentHead + 1) & (Size - 1);

        if (nextHead == tail_.load(std::memory_order_acquire)) {
            return false; // Buffer is full
        }

        buffer_[currentHead] = item;
        head_.store(nextHead, std::memory_order_release);
        return true;
    }

    // Returns the item if pop succeeded, nullopt if buffer is empty
    std::optional<T> pop() noexcept {
        const size_t currentTail = tail_.load(std::memory_order_relaxed);

        if (currentTail == head_.load(std::memory_order_acquire)) {
            return std::nullopt; // Buffer is empty
        }

        T item = buffer_[currentTail];
        tail_.store((currentTail + 1) & (Size - 1), std::memory_order_release);
        return item;
    }

    // Returns approximate number of items in buffer
    size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_relaxed);
        return (h - t) & (Size - 1);
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    bool full() const noexcept {
        const size_t nextHead = (head_.load(std::memory_order_relaxed) + 1) & (Size - 1);
        return nextHead == tail_.load(std::memory_order_relaxed);
    }

    static constexpr size_t capacity() noexcept {
        return Size - 1; // One slot is always empty to distinguish full from empty
    }

    void clear() noexcept {
        tail_.store(head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<size_t> head_; // Cache line aligned to prevent false sharing
    alignas(64) std::atomic<size_t> tail_;
    std::array<T, Size> buffer_;
};

} // namespace RLA
