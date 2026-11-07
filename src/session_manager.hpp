#pragma once

#include <libtorrent/session.hpp>

#include <memory>
#include <string>

namespace bt {

// RAII wrapper around a libtorrent session.
// Owns the session (via libtorrent::session, which *is* a session_handle),
// applies a sane default parameter set, and exposes the underlying handle for
// callers that need to add torrents or query state.
class SessionManager {
public:
    // resume_dir is where per-torrent resume data is persisted so that
    // completed (or in-progress) torrents reappear after a restart.
    explicit SessionManager(const std::string& resume_dir = "");
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    libtorrent::session_handle& session();
    const libtorrent::session_handle& session() const;

    // Restore any torrents with saved resume data. Called once at startup so
    // completed downloads persist across restarts.
    void restore_torrents();

    std::string resume_dir() const { return resume_dir_; }

private:
    std::unique_ptr<libtorrent::session> session_;
    std::string resume_dir_;
};

}  // namespace bt
