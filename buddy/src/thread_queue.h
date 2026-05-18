#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

template <typename T>
class ThreadQueue {
public:
    void push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
        cv_.notify_one();
    }

    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    std::optional<T> pop_wait(uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return !queue_.empty(); });
        }
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
};
