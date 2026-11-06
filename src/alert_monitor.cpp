#include "alert_monitor.hpp"

#include <libtorrent/alert_types.hpp>

#include <chrono>
#include <iostream>

namespace bt {

AlertMonitor::AlertMonitor(SessionManager& session) : session_(session) {}

AlertMonitor::~AlertMonitor() {
    stop();
}

void AlertMonitor::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) return;
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void AlertMonitor::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load() && !thread_.joinable()) return;
    running_.store(false);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool AlertMonitor::running() const {
    return running_.load();
}

void AlertMonitor::run() {
    while (running_.load()) {
        std::vector<libtorrent::alert*> alerts;
        session_.session().wait_for_alert(std::chrono::milliseconds(500));
        session_.session().pop_alerts(&alerts);

        for (libtorrent::alert* a : alerts) {
            if (!a || !running_.load()) continue;
            if (callback_) callback_(*a, describe(*a));
        }
    }
}

std::string AlertMonitor::describe(const libtorrent::alert& a) const {
    // Most alert types provide a human-readable message() override.
    switch (a.type()) {
        case libtorrent::add_torrent_alert::alert_type:
            return "torrent added";
        case libtorrent::torrent_finished_alert::alert_type:
            return "torrent finished";
        case libtorrent::tracker_error_alert::alert_type:
        case libtorrent::peer_error_alert::alert_type:
        case libtorrent::udp_error_alert::alert_type:
        case libtorrent::state_changed_alert::alert_type:
            return a.message();
        default:
            return "alert";
    }
}

}  // namespace bt
