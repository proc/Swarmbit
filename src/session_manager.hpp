#pragma once

#include <libtorrent/session.hpp>

#include <memory>

namespace bt {

// RAII wrapper around a libtorrent session.
// Owns the session (via libtorrent::session, which *is* a session_handle),
// applies a sane default parameter set, and exposes the underlying handle for
// callers that need to add torrents or query state.
class SessionManager {
public:
    SessionManager();
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    libtorrent::session_handle& session();
    const libtorrent::session_handle& session() const;

private:
    std::unique_ptr<libtorrent::session> session_;
};

}  // namespace bt
