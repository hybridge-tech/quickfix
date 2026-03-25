#ifndef FIX_SEND_QUEUE_H
#define FIX_SEND_QUEUE_H

#include <queue>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>

namespace FIX {

class Session;

class SendQueue {
public:
    struct QueuedOrder {
        std::vector<std::pair<int, std::string>> header;
        std::vector<std::pair<int, std::string>> body;
        std::chrono::steady_clock::time_point enqueue_time;
        std::string owner;
        std::string kind;
        int priority;
    };

    SendQueue() = default;
    ~SendQueue();

    void start(Session* session, int log_fd,
               int throttle_limit = 0, int spin_iterations = 1000);
    void stop();

    void push(std::vector<std::pair<int, std::string>> header,
              std::vector<std::pair<int, std::string>> body,
              int priority,
              std::string owner,
              std::string kind);

    size_t size() const { return m_size.load(std::memory_order_relaxed); }

private:
    void senderLoop();

    Session* m_session = nullptr;
    int m_logFd = -1;
    std::queue<QueuedOrder> m_hi;   // priority 0 (close orders)
    std::queue<QueuedOrder> m_lo;   // priority 1 (open orders)
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_sender;
    std::atomic<bool> m_running{false};
    std::atomic<size_t> m_size{0};
    int m_spin_iterations = 1000;

    // Throttle (only allocated/used when throttle_limit > 0)
    int m_throttle_limit = 0;
    std::deque<std::chrono::steady_clock::time_point> m_send_times;
};

} // namespace FIX
#endif