#include "session_manager.hpp"

#include <libtorrent/fingerprint.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/session_params.hpp>

#include <libtorrent/disk_interface.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <pwd.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace bt {

fs::path state_dir() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".swarmbit";
    }
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) {
        return fs::path(pw->pw_dir) / ".swarmbit";
    }
    std::error_code ec;
    return fs::current_path(ec) / ".swarmbit";
}

fs::path expand_path(const fs::path& path) {
    std::string s = path.string();
    if (s.empty()) return path;
    if (s[0] == '~') {
        fs::path home;
        if (const char* h = std::getenv("HOME"); h && *h) {
            home = h;
        } else if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) {
            home = pw->pw_dir;
        } else {
            std::error_code ec;
            home = fs::current_path(ec);
        }
        if (s.size() == 1) {
            return home;
        }
        if (s[1] == '/' || s[1] == '\\') {
            return home / s.substr(2);
        }
    }
    return path;
}

static fs::path config_file_path() {
    return state_dir() / "config";
}

fs::path default_download_dir() {
    return expand_path("~/Downloads");
}

fs::path load_config_download_dir() {
    fs::path cfg = config_file_path();
    std::error_code ec;
    if (!fs::exists(cfg, ec)) return "";

    std::ifstream f(cfg);
    if (!f.is_open()) return "";

    std::string line;
    while (std::getline(f, line)) {
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        if (line[start] == '#' || line[start] == ';') continue;

        auto eq = line.find_first_of("=:", start);
        if (eq == std::string::npos) continue;

        std::string key = line.substr(start, eq - start);
        auto key_end = key.find_last_not_of(" \t");
        if (key_end != std::string::npos) key = key.substr(0, key_end + 1);

        if (key == "download_dir" || key == "download-dir" || key == "download_path") {
            std::string val = line.substr(eq + 1);
            auto v_start = val.find_first_not_of(" \t");
            if (v_start != std::string::npos) {
                val = val.substr(v_start);
                auto v_end = val.find_last_not_of(" \t\r\n");
                if (v_end != std::string::npos) val = val.substr(0, v_end + 1);
            } else {
                val.clear();
            }
            if (!val.empty()) {
                return expand_path(val);
            }
        }
    }
    return "";
}

static void save_config_download_dir(const fs::path& path) {
    fs::path cfg = config_file_path();
    std::error_code ec;
    fs::create_directories(state_dir(), ec);

    std::vector<std::string> lines;
    bool found = false;
    if (fs::exists(cfg, ec)) {
        std::ifstream f(cfg);
        std::string line;
        while (std::getline(f, line)) {
            auto start = line.find_first_not_of(" \t");
            if (start != std::string::npos && line[start] != '#' && line[start] != ';') {
                auto eq = line.find_first_of("=:", start);
                if (eq != std::string::npos) {
                    std::string key = line.substr(start, eq - start);
                    auto key_end = key.find_last_not_of(" \t");
                    if (key_end != std::string::npos) key = key.substr(0, key_end + 1);
                    if (key == "download_dir" || key == "download-dir" || key == "download_path") {
                        lines.push_back("download_dir = " + path.string());
                        found = true;
                        continue;
                    }
                }
            }
            lines.push_back(line);
        }
    }
    if (!found) {
        lines.push_back("download_dir = " + path.string());
    }

    fs::path tmp = cfg;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        for (const auto& l : lines) {
            f << l << "\n";
        }
    }
    fs::rename(tmp, cfg, ec);
}

namespace {
std::mutex g_download_dir_mutex;
fs::path g_override_download_dir;
}  // namespace

void set_download_dir(const fs::path& path, bool persist) {
    std::lock_guard<std::mutex> lock(g_download_dir_mutex);
    g_override_download_dir = expand_path(path);
    if (persist) {
        save_config_download_dir(g_override_download_dir);
    }
}

fs::path download_dir() {
    {
        std::lock_guard<std::mutex> lock(g_download_dir_mutex);
        if (!g_override_download_dir.empty()) {
            return g_override_download_dir;
        }
    }

    if (const char* env = std::getenv("SWARMBIT_DOWNLOAD_DIR"); env && *env) {
        return expand_path(env);
    }
    if (const char* env = std::getenv("SWARMBIT_DOWNLOADS"); env && *env) {
        return expand_path(env);
    }

    fs::path from_config = load_config_download_dir();
    if (!from_config.empty()) {
        return from_config;
    }

    return default_download_dir();
}

SessionManager::SessionManager(const std::string& resume_dir)
    : resume_dir_(resume_dir.empty()
                      ? (state_dir() / "resume").string()
                      : resume_dir) {
    std::error_code ec;
    fs::create_directories(resume_dir_, ec);

    libtorrent::session_params params;
    params.disk_io_constructor = libtorrent::posix_disk_io_constructor;
    params.settings.set_str(
        libtorrent::settings_pack::user_agent, "Swarmbit/1.0.0");
    params.settings.set_str(
        libtorrent::settings_pack::handshake_client_version, "Swarmbit/1.0.0");
    params.settings.set_str(
        libtorrent::settings_pack::peer_fingerprint,
        libtorrent::generate_fingerprint("SB", 1, 0, 0));
    params.settings.set_int(
        libtorrent::settings_pack::alert_mask,
        libtorrent::alert_category::error |
        libtorrent::alert_category::status |
        libtorrent::alert_category::storage |  // save_resume_data_alert
        libtorrent::alert_category::peer |
        libtorrent::alert_category::tracker);

    // Cap upload (seed) rate at 100 KB/s.
    params.settings.set_int(
        libtorrent::settings_pack::upload_rate_limit, 800 * 1024);
    params.settings.set_int(
        libtorrent::settings_pack::connections_limit, 200);
    params.settings.set_int(
        libtorrent::settings_pack::initial_picker_threshold, 0);
    params.settings.set_bool(
        libtorrent::settings_pack::auto_sequential, true);
    params.settings.set_bool(
        libtorrent::settings_pack::piece_extent_affinity, false);
    params.settings.set_bool(
        libtorrent::settings_pack::close_redundant_connections, false);

    session_ = std::make_unique<libtorrent::session>(std::move(params));

    // Enable per-torrent status updates so tracker scrape data populates
    // num_complete / num_incomplete (seed / leecher counts). Without this,
    // these fields stay -1 until a tracker announce includes scrape info.
    session_->post_torrent_updates(libtorrent::status_flags_t::all());
}

SessionManager::~SessionManager() {
    session_->pause();
    session_.reset();
}

libtorrent::session_handle& SessionManager::session() {
    return *session_;
}

const libtorrent::session_handle& SessionManager::session() const {
    return *session_;
}

void SessionManager::restore_torrents() {
    if (!fs::exists(resume_dir_)) return;

    for (const auto& entry : fs::directory_iterator(resume_dir_)) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".resume") {
            continue;
        }

        try {
            std::ifstream f(entry.path(), std::ios::binary);
            std::string buffer((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
            if (buffer.empty()) continue;

            boost::system::error_code ec;
            auto params = libtorrent::read_resume_data(
                libtorrent::span<char const>(buffer.data(), buffer.size()),
                ec);
            if (ec) continue;

            // Skip torrents already present in the session.
            auto hashes = params.info_hashes;
            if (hashes.v1 != libtorrent::sha1_hash() &&
                session().find_torrent(hashes.v1).is_valid()) {
                continue;
            }
            if (hashes.v2 != libtorrent::sha256_hash() &&
                session().find_torrent(hashes.v2).is_valid()) {
                continue;
            }

            // Preserve the original save path so downloaded files stay put
            // instead of being re-downloaded.
            if (params.save_path.empty() && !params.ti) {
                continue;  // Magnet without a resolved path can't restore.
            }

            // Restore with the flags captured in the resume data, so a
            // torrent comes back paused only if it was paused when saved, and
            // completed torrents keep seeding.
            session().add_torrent(params, ec);
            if (ec) {
                // Resume data may be stale; leave it on disk.
            }
        } catch (const std::exception&) {
            // Ignore malformed resume files.
        }
    }
}

}  // namespace bt
