/* -*- C++ -*- */

#ifndef FIX_MESSAGE_QUEUE_H
#define FIX_MESSAGE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <atomic>
#include <cstdint>
#include <optional>

namespace FIX {

/**
 * High-performance message queue for low-latency FIX message delivery.
 * Pure C++ implementation - Python bindings are in quickfix.i
 */
class MessageQueue {
public:
    struct QueuedMessage {
        std::string msgType;
        std::string raw;
        int64_t receive_time_ns;
    };

    static MessageQueue& instance() {
        static MessageQueue inst;
        return inst;
    }

    // Push a message onto the queue (called from C++ Session).
    // Takes strings by value so callers can move; the queue node is built
    // outside the lock and moved in — no allocation in the critical section.
    void push(std::string msgType, std::string raw, int64_t receive_time_ns) {
        QueuedMessage qm{std::move(msgType), std::move(raw), receive_time_ns};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(std::move(qm));
            m_size.store(m_queue.size(), std::memory_order_release);
        }
        m_cv.notify_one();
    }

    // Pop next message (non-blocking)
    // Returns nullopt if queue is empty
    std::optional<QueuedMessage> pop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return std::nullopt;
        }
        QueuedMessage msg = std::move(m_queue.front());
        m_queue.pop();
        m_size.store(m_queue.size(), std::memory_order_release);
        return msg;
    }

    // Wait for next message with timeout (milliseconds)
    // Returns nullopt if timeout
    // Spin-then-wait (same pattern as SendQueue): condvar wake-ups cost
    // 100-250us on virtualized/non-pinned cores; a short pause-spin on the
    // atomic size counter catches back-to-back messages without sleeping.
    std::optional<QueuedMessage> waitAndPop(int timeout_ms) {
        if (m_size.load(std::memory_order_acquire) == 0) {
            for (int i = 0; i < m_spin_iterations; ++i) {
                if (m_size.load(std::memory_order_relaxed) > 0)
                    break;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                asm volatile("pause" ::: "memory");
#else
                asm volatile("" ::: "memory");
#endif
            }
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
                return !m_queue.empty();
            });
        }
        if (m_queue.empty()) {
            return std::nullopt;
        }
        QueuedMessage msg = std::move(m_queue.front());
        m_queue.pop();
        m_size.store(m_queue.size(), std::memory_order_release);
        return msg;
    }

    // Spin iterations before falling back to condvar in waitAndPop
    // (0 = pure condvar, previous behavior)
    void setSpinIterations(int n) { m_spin_iterations = n > 0 ? n : 0; }

    // Check if queue has messages (non-blocking)
    bool hasMessages() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_queue.empty();
    }

    // Get queue size
    size_t size() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    // Get reference to mutex for external GIL handling
    std::mutex& mutex() { return m_mutex; }
    std::condition_variable& cv() { return m_cv; }

private:
    MessageQueue() = default;
    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;

    std::queue<QueuedMessage> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<size_t> m_size{0};
    int m_spin_iterations = 1000;  // same default as SendQueue
};

// Free functions for C++ usage
inline bool hasMessages() {
    return MessageQueue::instance().hasMessages();
}

inline size_t messageQueueSize() {
    return MessageQueue::instance().size();
}

} // namespace FIX

#endif // FIX_MESSAGE_QUEUE_H