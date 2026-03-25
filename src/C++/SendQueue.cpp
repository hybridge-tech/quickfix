#ifdef _MSC_VER
#include "stdafx.h"
#else
#include "config.h"
#endif

#include "SendQueue.h"
#include "Session.h"
#include <ctime>

namespace FIX {

SendQueue::~SendQueue() { stop(); }

void SendQueue::start(Session* session, int log_fd,
                      int throttle_limit, int spin_iterations) {
    m_session = session;
    m_throttle_limit = throttle_limit;
    m_spin_iterations = spin_iterations;
    if (log_fd >= 0) {
        m_logFd = ::dup(log_fd);  // Own copy — survives Python's close()
    }
    m_running = true;
    m_sender = std::thread(&SendQueue::senderLoop, this);
}

void SendQueue::stop() {
    m_running = false;
    m_cv.notify_one();
    if (m_sender.joinable())
        m_sender.join();
    if (m_logFd >= 0) {
        ::close(m_logFd);
        m_logFd = -1;
    }
}

void SendQueue::push(
    std::vector<std::pair<int, std::string>> header,
    std::vector<std::pair<int, std::string>> body,
    int priority,
    std::string owner,
    std::string kind)
{
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (priority == 0)
            m_hi.push({std::move(header), std::move(body), now,
                       std::move(owner), std::move(kind), priority});
        else
            m_lo.push({std::move(header), std::move(body), now,
                       std::move(owner), std::move(kind), priority});
    }
    m_size.fetch_add(1, std::memory_order_relaxed);
    m_cv.notify_one();
}

void SendQueue::senderLoop() {
    while (m_running) {
        QueuedOrder order;
        {
            std::unique_lock<std::mutex> lk(m_mutex);

            // Spin-wait: check without sleeping to avoid condvar wake-up latency
            if (m_hi.empty() && m_lo.empty()) {
                lk.unlock();
                for (int i = 0; i < m_spin_iterations; ++i) {
                    if (m_size.load(std::memory_order_relaxed) > 0 || !m_running)
                        break;
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
                    asm volatile("pause" ::: "memory");
#else
                    asm volatile("" ::: "memory");
#endif
                }
                lk.lock();
            }

            // If still empty after spin, fall back to condvar
            if (m_hi.empty() && m_lo.empty()) {
                m_cv.wait(lk, [this] {
                    return !m_hi.empty() || !m_lo.empty() || !m_running;
                });
            }
            if (!m_running && m_hi.empty() && m_lo.empty()) break;

            if (!m_hi.empty()) {
                order = std::move(m_hi.front());
                m_hi.pop();
            } else {
                order = std::move(m_lo.front());
                m_lo.pop();
            }
        }
        m_size.fetch_sub(1, std::memory_order_relaxed);

        // Throttle (only when configured, zero overhead otherwise)
        if (m_throttle_limit > 0) {
            auto now = std::chrono::steady_clock::now();
            auto cutoff = now - std::chrono::seconds(1);
            while (!m_send_times.empty() && m_send_times.front() < cutoff)
                m_send_times.pop_front();
            if (static_cast<int>(m_send_times.size()) >= m_throttle_limit) {
                auto sleep_until = m_send_times.front() + std::chrono::seconds(1);
                m_send_times.pop_front();
                auto before = std::chrono::steady_clock::now();
                std::this_thread::sleep_until(sleep_until);
                if (m_logFd >= 0) {
                    auto slept_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - before).count();
                    auto wall = std::chrono::system_clock::now();
                    auto tt = std::chrono::system_clock::to_time_t(wall);
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        wall.time_since_epoch()) % 1000;
                    struct tm local;
                    localtime_r(&tt, &local);
                    char buf[256];
                    int n = snprintf(buf, sizeof(buf),
                        "%04d-%02d-%02d %02d:%02d:%02d,%03d WARNING (SendQueue) SendQueue.cpp:%-3d -> "
                        "throttle %d/%d slept %.5fs\n",
                        local.tm_year+1900, local.tm_mon+1, local.tm_mday,
                        local.tm_hour, local.tm_min, local.tm_sec,
                        static_cast<int>(ms.count()),
                        __LINE__,
                        static_cast<int>(m_send_times.size()), m_throttle_limit,
                        slept_us / 1000000.0);
                    if (n > 0) { auto w = ::write(m_logFd, buf, n); (void)w; }
                }
            }
            m_send_times.push_back(std::chrono::steady_clock::now());
        }

        // encodeAndSend handles tag 52 (SendingTime) injection internally
        m_session->encodeAndSend(order.header, order.body);

        // Log latency — POSIX write() is atomic for < PIPE_BUF, no buffering
        if (m_logFd >= 0) {
            auto sent_time = std::chrono::steady_clock::now();
            auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
                sent_time - order.enqueue_time).count();

            auto wall = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(wall);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                wall.time_since_epoch()) % 1000;
            struct tm local;
            localtime_r(&tt, &local);

            auto qsz = m_size.load(std::memory_order_relaxed);
            double latency_s = latency_us / 1000000.0;

            char buf[512];
            int n = snprintf(buf, sizeof(buf),
                "%04d-%02d-%02d %02d:%02d:%02d,%03d WARNING (SendQueue) SendQueue.cpp:%-3d -> "
                "%s sent %s [%s] size %zu %.5fs\n",
                local.tm_year+1900, local.tm_mon+1, local.tm_mday,
                local.tm_hour, local.tm_min, local.tm_sec,
                static_cast<int>(ms.count()),
                __LINE__,
                order.owner.c_str(), order.kind.c_str(),
                (order.priority == 0) ? "HI" : "LO",
                qsz, latency_s);
            if (n > 0) { auto w = ::write(m_logFd, buf, n); (void)w; }
        }
    }
}

} // namespace FIX