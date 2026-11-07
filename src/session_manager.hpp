#pragma once

#include <libtorrent/session.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace bt {

// Per-user state directory: `~/.swarmbit` ($HOME, falling back to the
// password database, then the working directory if neither is available).
// Holds resume data, the downloading list, and cached .torrent files.
std::filesystem::path state_dir();

// Expands leading '~' or '~/...' to the user's home directory.
std::filesystem::path expand_path(const std::filesystem::path& path);

// Returns the default download directory: `~/Downloads` ($HOME/Downloads).
std::filesystem::path default_download_dir();

// Returns the effective download directory:
// 1. Programmatic override (via set_download_dir)
// 2. SWARMBIT_DOWNLOAD_DIR / SWARMBIT_DOWNLOADS environment variable
// 3. ~/.swarmbit/config (setting: download_dir = ...)
// 4. default_download_dir() (~/Downloads)
std::filesystem::path download_dir();

// Sets a programmatic override for the download directory. If persist is true,
// saves it to ~/.swarmbit/config.
void set_download_dir(const std::filesystem::path& path, bool persist = false);

// Loads the download directory from ~/.swarmbit/config, returning empty if not configured.
std::filesystem::path load_config_download_dir();

// RAII wrapper around a libtorrent session.
// Owns the session (via libtorrent::session, which *is* a session_handle),
// applies a sane default parameter set, and exposes the underlying handle for
// callers that need to add torrents or query state.
class SessionManager {
public:
    // resume_dir is where per-torrent resume data is persisted so that
    // completed (or in-progress) torrents reappear after a restart. Defaults
    // to `~/.swarmbit/resume`, the same directory the dashboard uses.
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
