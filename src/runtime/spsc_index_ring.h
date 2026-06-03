#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace voxinsert {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

class SpscIndexRing {
public:
    explicit SpscIndexRing(size_t capacity)
        : buffer_(capacity + 1), capacity_(capacity + 1) {}

    ~SpscIndexRing() = default;

    SpscIndexRing(const SpscIndexRing&) = delete;
    SpscIndexRing& operator=(const SpscIndexRing&) = delete;
    SpscIndexRing(SpscIndexRing&&) = delete;
    SpscIndexRing& operator=(SpscIndexRing&&) = delete;

    bool TryPush(size_t value) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t nextTail = Increment(tail);
        if (nextTail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[tail] = value;
        tail_.store(nextTail, std::memory_order_release);
        return true;
    }

    bool TryPop(size_t& value) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        value = buffer_[head];
        head_.store(Increment(head), std::memory_order_release);
        return true;
    }

    void Reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    size_t Size() const noexcept {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return tail >= head ? tail - head : capacity_ - head + tail;
    }

    size_t Capacity() const noexcept { return capacity_ - 1; }

private:
    size_t Increment(size_t index) const noexcept {
        ++index;
        return index == capacity_ ? 0 : index;
    }

    std::vector<size_t> buffer_;
    const size_t capacity_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace voxinsert
