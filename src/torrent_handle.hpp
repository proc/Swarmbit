#pragma once

#include "session_manager.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>

#include <string>
#include <vector>

namespace bt {

// Handle to a torrent added to the session. Used to query state and drive the
// torrent lifecycle (pause/resume/remove).
class Torrent {
public:
    Torrent(SessionManager& session, libtorrent::torrent_handle handle);

    // Add a .torrent file or magnet link to the session. On success returns a
    // Torrent wrapping the new handle. On failure throws std::runtime_error
    // with the libtorrent error message.
    static Torrent add_file(SessionManager& session, const std::string& torrent_path);
    static Torrent add_magnet(SessionManager& session, const std::string& magnet_uri);

    const libtorrent::torrent_handle& handle() const;
    bool is_valid() const;
    bool is_finished() const;  // all wanted pieces downloaded

    enum class State { NotAdded, Downloading, Seeding, Completed, Checking };
    State state() const;

    // Progress in [0, 1], or -1 if the torrent is invalid.
    double progress() const;

    // Download / upload rate in bytes per second.
    long long download_rate() const;
    long long upload_rate() const;

    // Peer and piece statistics.
    int num_peers() const;
    int num_complete() const;   // seeders known to DHT/trackers
    int num_incomplete() const; // leechers known to DHT/trackers

    // Cumulative payload downloaded / uploaded in bytes (per active session).
    long long total_download() const;
    long long total_upload() const;

    std::string name() const;
    std::string info_hash() const;
    std::string save_path() const;

    void pause();
    void resume();
    void remove(bool delete_files);

    // File listing for this torrent.
    struct FileInfo {
        std::string path;
        long long size;
        double progress;  // fraction of this file downloaded
    };
    std::vector<FileInfo> files() const;

private:
    libtorrent::torrent_status status() const;

    SessionManager& session_;
    libtorrent::torrent_handle handle_;
};

}  // namespace bt
