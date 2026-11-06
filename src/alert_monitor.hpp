#pragma once

#include "session_manager.hpp"

#include <libtorrent/alert.hpp>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace bt {

// Background thread that drains the session's alert queue and forwards alerts
// to a user callback. Alerts are the only reliable way to observe torrent
// state changes (added, finished, paused, errors) in libtorrent.
class AlertMonitor {
public:
    using Callback =
        std::function<void(const libtorrent::alert&, const std::string&)>;

    explicit AlertMonitor(SessionManager& session);
    ~AlertMonitor();

    AlertMonitor(const AlertMonitor&) = delete;
    AlertMonitor& operator=(const AlertMonitor&) = delete;

    void start();
    void stop();
    bool running() const;

private:
    void run();
    std::string describe(const libtorrent::alert& a) const;

    SessionManager& session_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    Callback callback_;
};

}  // namespace bt
