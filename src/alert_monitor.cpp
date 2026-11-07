#include "alert_monitor.hpp"

#include <libtorrent/alert_types.hpp>
#include <libtorrent/hex.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>

namespace bt {

namespace fs = std::filesystem;

// Encode bytes as a hex string for use in filenames.
std::string to_hex_string(const char* data, std::size_t len) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        unsigned byte = static_cast<unsigned>(static_cast<unsigned char>(data[i]));
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xF]);
    }
    return out;
}

AlertMonitor::AlertMonitor(SessionManager& session)
    : session_(session), resume_dir_(session.resume_dir()) {}

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

void AlertMonitor::set_resume_dir(const fs::path& dir) {
    resume_dir_ = dir;
}

namespace {
// How often run() checks for torrents whose progress/state changed since
// their resume data was last written.
constexpr auto kPeriodicSaveInterval = std::chrono::seconds(30);
}  // namespace

void AlertMonitor::run() {
    last_periodic_save_ = std::chrono::steady_clock::now();
    while (running_.load()) {
        std::vector<libtorrent::alert*> alerts;
        session_.session().wait_for_alert(std::chrono::milliseconds(500));
        session_.session().pop_alerts(&alerts);

        for (libtorrent::alert* a : alerts) {
            if (!a || !running_.load()) continue;
            handle_alert(a);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_periodic_save_ >= kPeriodicSaveInterval) {
            last_periodic_save_ = now;
            save_changed_torrents();
        }
    }
}

void AlertMonitor::handle_alert(const libtorrent::alert* a) {
    if (callback_) callback_(*a, describe(*a));

    // Persist resume data so in-progress/completed torrents reappear after a
    // restart. Keyed by info-hash so restore_torrents() can find it in the
    // resume dir.
    if (auto* sa = libtorrent::alert_cast<libtorrent::save_resume_data_alert>(a)) {
        // libtorrent may deliver resume data after torrent_removed_alert;
        // don't resurrect a torrent the user just removed.
        const auto best = sa->params.info_hashes.get_best();
        if (session_.session().find_torrent(best).is_valid()) {
            write_resume_file(sa->params);
        }
        return;
    }

    // A removed torrent must not be restored on the next launch.
    if (auto* ra = libtorrent::alert_cast<libtorrent::torrent_removed_alert>(a)) {
        std::error_code ec;
        fs::remove(resume_file_path(ra->info_hashes), ec);
        return;
    }

    // Save promptly on lifecycle events rather than waiting for the periodic
    // sweep: a new torrent (with its metadata), a magnet that just resolved,
    // a finished download, and pause/resume (the paused flag is restored).
    if (auto* aa = libtorrent::alert_cast<libtorrent::add_torrent_alert>(a)) {
        if (!aa->error) request_save(aa->handle);
    } else if (auto* ma = libtorrent::alert_cast<libtorrent::metadata_received_alert>(a)) {
        request_save(ma->handle);
    } else if (auto* fa = libtorrent::alert_cast<libtorrent::torrent_finished_alert>(a)) {
        request_save(fa->handle);
    } else if (auto* pa = libtorrent::alert_cast<libtorrent::torrent_paused_alert>(a)) {
        request_save(pa->handle);
    } else if (auto* ua = libtorrent::alert_cast<libtorrent::torrent_resumed_alert>(a)) {
        request_save(ua->handle);
    }
}

void AlertMonitor::request_save(const libtorrent::torrent_handle& h) const {
    if (!h.is_valid()) return;
    try {
        // Embed the info dict so magnet torrents restore without refetching
        // metadata and without depending on the cached .torrent file.
        h.save_resume_data(libtorrent::torrent_handle::save_info_dict);
    } catch (const std::exception&) {
        // Handle became invalid (torrent removed); nothing to save.
    }
}

void AlertMonitor::save_changed_torrents() const {
    // Skip if_counters_changed: active-time counters tick constantly, which
    // would rewrite every seeding torrent's file on every sweep.
    const auto changed = libtorrent::torrent_handle::if_download_progress |
                         libtorrent::torrent_handle::if_state_changed |
                         libtorrent::torrent_handle::if_config_changed |
                         libtorrent::torrent_handle::if_metadata_changed;
    try {
        for (const auto& h : session_.session().get_torrents()) {
            if (h.is_valid() && h.need_save_resume_data(changed)) {
                request_save(h);
            }
        }
    } catch (const std::exception&) {
    }
}

int AlertMonitor::save_all_resume_data(std::chrono::seconds timeout) {
    int outstanding = 0;
    try {
        for (const auto& h : session_.session().get_torrents()) {
            if (!h.is_valid()) continue;
            try {
                h.save_resume_data(libtorrent::torrent_handle::save_info_dict |
                                   libtorrent::torrent_handle::flush_disk_cache);
                ++outstanding;
            } catch (const std::exception&) {
            }
        }
    } catch (const std::exception&) {
    }

    int written = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (outstanding > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        if (!session_.session().wait_for_alert(wait)) break;

        std::vector<libtorrent::alert*> alerts;
        session_.session().pop_alerts(&alerts);
        for (libtorrent::alert* a : alerts) {
            if (!a) continue;
            if (auto* sa =
                    libtorrent::alert_cast<libtorrent::save_resume_data_alert>(a)) {
                --outstanding;
                if (write_resume_file(sa->params)) ++written;
                if (callback_) callback_(*a, describe(*a));
            } else if (libtorrent::alert_cast<
                           libtorrent::save_resume_data_failed_alert>(a)) {
                --outstanding;
                if (callback_) callback_(*a, describe(*a));
            } else {
                // Lifecycle alerts would request more saves; just forward.
                if (callback_) callback_(*a, describe(*a));
                if (auto* ra = libtorrent::alert_cast<
                        libtorrent::torrent_removed_alert>(a)) {
                    std::error_code ec;
                    fs::remove(resume_file_path(ra->info_hashes), ec);
                }
            }
        }
    }
    return written;
}

fs::path AlertMonitor::resume_file_path(
    const libtorrent::info_hash_t& hashes) const {
    const std::string key =
        to_hex_string(hashes.v1.data(), hashes.v1.size()) + "_" +
        to_hex_string(hashes.v2.data(), hashes.v2.size());
    return resume_dir_ / (key + ".resume");
}

bool AlertMonitor::write_resume_file(
    const libtorrent::add_torrent_params& params) const {
    try {
        std::error_code ec;
        fs::create_directories(resume_dir_, ec);
        const std::vector<char> buf = libtorrent::write_resume_data_buf(params);
        const fs::path dst = resume_file_path(params.info_hashes);
        // Write to a temp file and rename so a crash mid-write can't leave a
        // truncated resume file behind.
        fs::path tmp = dst;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            if (!out) return false;
        }
        fs::rename(tmp, dst, ec);
        return !ec;
    } catch (const std::exception&) {
        // Best-effort: a failed resume write must not disturb the UI.
        return false;
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
