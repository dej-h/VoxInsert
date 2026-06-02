#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <stop_token>
#include <utility>

namespace voxinsert {

// A bounded, thread-safe FIFO queue that stores owned values.
//
// Usage rules from the streaming plan:
//   - Producers that must not block (the audio thread) use TryPush only.
//   - The coordinator consumes with WaitPop and honors a std::stop_token.
//   - Close wakes all waiters so workers can unblock during shutdown.
template <typename T>
class BlockingBoundedQueue {
public:
    explicit BlockingBoundedQueue(size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    BlockingBoundedQueue(const BlockingBoundedQueue&) = delete;
    BlockingBoundedQueue& operator=(const BlockingBoundedQueue&) = delete;

    // Non-blocking. Returns false if the queue is full or closed; the value is
    // left untouched so the caller can keep ownership.
    bool TryPush(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || queue_.size() >= capacity_) {
                return false;
            }
            queue_.push(std::move(value));
        }
        notEmpty_.notify_one();
        return true;
    }

    // Blocks until a value is available, the queue is closed, or the stop token
    // is signaled. Returns false when no value could be popped (closed+empty or
    // stop requested).
    bool WaitPop(T& value, std::stop_token stopToken) {
        // Register the wake callback before acquiring the lock. If stop was
        // already requested, the callback runs inline on this thread; doing so
        // outside the lock avoids a self-deadlock on the non-recursive mutex.
        // The callback briefly takes the mutex so its notify cannot slip
        // between the waiter's predicate check and its wait.
        std::stop_callback stopCallback(stopToken, [this] {
            { std::lock_guard<std::mutex> wakeLock(mutex_); }
            notEmpty_.notify_all();
        });

        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [&] {
            return !queue_.empty() || closed_ || stopToken.stop_requested();
        });

        if (queue_.empty()) {
            return false;
        }

        value = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        notFull_.notify_one();
        return true;
    }

    void Close() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    // Re-opens a previously closed queue and discards any leftover items so the
    // instance can be reused for a new session. Must only be called when no
    // producer or consumer is touching the queue (i.e. between sessions, after
    // the pump threads have been joined).
    void Reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        queue_.swap(empty);
        closed_ = false;
    }

    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::queue<T> queue_;
    const size_t capacity_;
    bool closed_ = false;
};

} // namespace voxinsert
