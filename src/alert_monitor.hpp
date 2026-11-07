#pragma once

#include "session_manager.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/info_hash.hpp>
#include <libtorrent/torrent_handle.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace bt {

// Background thread that drains the session's alert queue and forwards alerts
// to a user callback. Alerts are the only reliable way to observe torrent
// state changes (added, finished, paused, errors) in libtorrent.
//
// It also owns fast-resume persistence: it asks libtorrent for resume data
// when a torrent is added, receives metadata, finishes, or is paused/resumed,
// and periodically for torrents with unsaved progress. The resulting
// save_resume_data_alerts are written to the resume directory (one
// `<v1hex>_<v2hex>.resume` file per torrent) so in-progress and completed
// torrents can be restored after a restart via
// SessionManager::restore_torrents(). A torrent's resume file is deleted when
// the torrent is removed from the session.
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

    // Set the alert callback used to forward alerts to the caller.
    void set_callback(Callback cb) { callback_ = std::move(cb); }

    // Directory where per-torrent resume data is written. Must match the
    // SessionManager's resume_dir_ so restore_torrents() can find the files.
    void set_resume_dir(const std::filesystem::path& dir);

    // Request resume data for every torrent in the session and block until
    // it has all been written (or `timeout` elapses). Call on shutdown, after
    // stop(), and before the session is destroyed. Returns the number of
    // resume files written.
    int save_all_resume_data(
        std::chrono::seconds timeout = std::chrono::seconds(10));

private:
    void run();
    std::string describe(const libtorrent::alert& a) const;

    // Shared per-alert handling for run() and save_all_resume_data().
    void handle_alert(const libtorrent::alert* a);
    void request_save(const libtorrent::torrent_handle& h) const;
    void save_changed_torrents() const;
    bool write_resume_file(const libtorrent::add_torrent_params& params) const;
    std::filesystem::path resume_file_path(
        const libtorrent::info_hash_t& hashes) const;

    SessionManager& session_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    Callback callback_;
    std::filesystem::path resume_dir_;
    std::chrono::steady_clock::time_point last_periodic_save_{};
};

}  // namespace bt
