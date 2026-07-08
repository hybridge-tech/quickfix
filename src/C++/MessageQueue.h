/* -*- C++ -*- */

#ifndef FIX_MESSAGE_QUEUE_H
#define FIX_MESSAGE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
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
        return msg;
    }

    // Wait for next message with timeout (milliseconds)
    // Returns nullopt if timeout
    std::optional<QueuedMessage> waitAndPop(int timeout_ms) {
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
        return msg;
    }

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