// Minimal self-contained checks for the torrent client library.
// These do not require network access: they validate magnet-link parsing and
// info-hash hex helpers, which are pure functions.

#include "alert_monitor.hpp"
#include "session_manager.hpp"
#include "torrent_handle.hpp"

#include <libtorrent/create_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session_handle.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

int failures = 0;
void check(bool cond, const std::string& what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    } else {
        std::cout << "ok: " << what << "\n";
    }
}

// Local hex helpers mirroring the client implementation, to avoid the hidden
// libtorrent::aux::to_hex / from_hex symbols in the Homebrew .dylib.
std::string to_hex(const char* data, std::size_t len) {
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

bool from_hex(const std::string& hex, char* out, std::size_t len) {
    if (hex.size() != len * 2) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < len; ++i) {
        int hi = digit(hex[2 * i]);
        int lo = digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<char>((hi << 4) | lo);
    }
    return true;
}

void test_magnet_parse() {
    const std::string uri =
        "magnet:?xt=urn:btih:0677fa53cba24a762920904ad1f4237f57812604"
        "&dn=Test&tr=http://tracker.example/announce";
    auto p = libtorrent::parse_magnet_uri(uri);
    check(p.info_hashes.v1.size() == 20, "magnet: parsed info_hash is 20 bytes");
}

void test_hex_roundtrip() {
    std::array<char, 20> bytes{};
    for (int i = 0; i < 20; ++i) bytes[i] = static_cast<char>(i);
    const std::string hex = to_hex(bytes.data(), 20);
    check(hex.size() == 40, "to_hex produces 40 chars for 20 bytes");

    libtorrent::sha1_hash out;
    check(from_hex(hex, out.data(), 20), "from_hex round-trips the bytes");
    check(std::string(out.data(), 20) == std::string(bytes.data(), 20),
          "from_hex output matches original bytes");
}

// Real end-to-end check: add a torrent to a live session and confirm the
// find_torrent/get_torrents API that backs the `list`/`status` commands works.
// The info-hash below is the Ubuntu 22.04.5 desktop ISO; the check only needs
// the handle to be valid and discoverable, not a full download.
void test_find_torrent() {
    bt::SessionManager session;
    auto& sh = session.session();

    libtorrent::add_torrent_params params;
    params.info_hashes = libtorrent::info_hash_t(
        libtorrent::sha1_hash("2e8e44068b254814ea1a7d4969a9af1d78e0f51f"));
    params.save_path = "/tmp/bt-test-save";

    libtorrent::torrent_handle h = sh.add_torrent(params);
    check(h.is_valid(), "add_torrent returns a valid handle");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto torrents = sh.get_torrents();
    check(torrents.size() == 1, "get_torrents returns the added torrent");

    libtorrent::torrent_handle found = sh.find_torrent(h.info_hash());
    check(found.is_valid(), "find_torrent locates the added torrent");

    // Without metadata (and no peers on a test box) the ETA can't be known.
    bt::Torrent t(session, found);
    check(t.eta_seconds() == -1, "eta_seconds is unknown before metadata");
    check(t.bytes_remaining() >= 0, "bytes_remaining is non-negative");
}

// Write a tiny single-file payload into `dir` and return add_torrent_params
// that seed it from there. Fully offline: the data already exists on disk,
// so libtorrent just hash-checks it.
libtorrent::add_torrent_params make_local_torrent(
    const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "payload.bin";
    {
        std::ofstream out(file, std::ios::binary);
        std::string chunk(64 * 1024, 'x');
        for (int i = 0; i < 4; ++i) out << chunk;
    }

    auto files = libtorrent::list_files(file.string());
    libtorrent::create_torrent ct(files);
    libtorrent::set_piece_hashes(ct, dir.string());
    std::vector<char> buf = ct.generate_buf();

    libtorrent::add_torrent_params params;
    params.ti = std::make_shared<libtorrent::torrent_info>(
        libtorrent::span<char const>(buf), libtorrent::from_span);
    params.save_path = dir.string();
    return params;
}

// Wait for the hash check to finish so the torrent is seeding.
void wait_for_seeding(const libtorrent::torrent_handle& h) {
    for (int i = 0; i < 50 && !h.status().is_seeding; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Seed a local torrent, then remove it via bt::Torrent::remove(). Returns
// whether the payload still exists afterwards.
bool remove_test_case(bool delete_files) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
                         (delete_files ? "swarmbit-rm-files" : "swarmbit-rm-keep");
    const fs::path file = dir / "payload.bin";
    auto params = make_local_torrent(dir);

    bt::SessionManager session;
    libtorrent::torrent_handle h = session.session().add_torrent(params);
    wait_for_seeding(h);

    bt::Torrent t(session, h);
    t.remove(delete_files);

    // Deletion is asynchronous; poll for the outcome.
    for (int i = 0; i < 50; ++i) {
        if (!fs::exists(file)) break;
        if (!delete_files && session.session().get_torrents().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool exists = fs::exists(file);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return exists;
}

void test_remove() {
    check(!remove_test_case(true), "remove(true) deletes downloaded data");
    check(remove_test_case(false), "remove(false) keeps downloaded data");
}

std::size_t count_resume_files(const std::filesystem::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".resume") ++n;
    }
    return n;
}

// Resume data round trip: AlertMonitor writes a .resume file, a fresh session
// restores the torrent from it with its saved paused state, and removing the
// torrent deletes the file so it isn't restored again.
void test_resume_persistence() {
    namespace fs = std::filesystem;
    const fs::path data_dir = fs::temp_directory_path() / "swarmbit-resume-data";
    const fs::path resume_dir = fs::temp_directory_path() / "swarmbit-resume-dir";
    std::error_code ec;
    fs::remove_all(resume_dir, ec);
    auto params = make_local_torrent(data_dir);
    const auto ih = params.ti->info_hashes();

    {
        bt::SessionManager session(resume_dir.string());
        bt::AlertMonitor monitor(session);
        monitor.start();
        libtorrent::torrent_handle h = session.session().add_torrent(params);
        wait_for_seeding(h);
        h.pause();  // the paused state should survive the restart

        // Lifecycle alerts (added/finished/paused) trigger saves on their own.
        for (int i = 0; i < 50 && count_resume_files(resume_dir) == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        check(count_resume_files(resume_dir) == 1,
              "resume file written while running");

        monitor.stop();
        check(monitor.save_all_resume_data() == 1,
              "save_all_resume_data flushes every torrent on shutdown");
    }

    {
        bt::SessionManager session(resume_dir.string());
        session.restore_torrents();
        libtorrent::torrent_handle h = session.session().find_torrent(ih.get_best());
        check(h.is_valid(), "restore_torrents restores the saved torrent");
        check(h.is_valid() &&
                  static_cast<bool>(h.flags() & libtorrent::torrent_flags::paused),
              "restored torrent keeps its saved paused state");

        bt::Torrent t(session, h);
        check(t.is_finished(), "restored torrent is finished");
        check(t.progress() == 1.0, "restored completed torrent progress is 1.0");

        bt::AlertMonitor monitor(session);
        monitor.start();
        t.remove(false);
        for (int i = 0; i < 50 && count_resume_files(resume_dir) != 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        check(count_resume_files(resume_dir) == 0,
              "removing a torrent deletes its resume file");
        monitor.stop();
    }

    fs::remove_all(data_dir, ec);
    fs::remove_all(resume_dir, ec);
}

}  // namespace

void test_state_dir() {
    auto dir = bt::state_dir();
    check(!dir.empty(), "state_dir is not empty");
    check(dir.filename() == ".swarmbit", "state_dir filename is .swarmbit");
    const char* home = std::getenv("HOME");
    if (home && *home) {
        check(dir == std::filesystem::path(home) / ".swarmbit",
              "state_dir matches $HOME/.swarmbit");
    }
}

void test_download_dir() {
    // 1. Default download directory should end in "Downloads" and match ~/Downloads
    auto def_dir = bt::default_download_dir();
    check(!def_dir.empty(), "default_download_dir is not empty");
    check(def_dir.filename() == "Downloads", "default_download_dir filename is Downloads");
    const char* home = std::getenv("HOME");
    if (home && *home) {
        check(def_dir == std::filesystem::path(home) / "Downloads",
              "default_download_dir matches $HOME/Downloads");
    }

    // 2. expand_path tests
    check(bt::expand_path("/absolute/path") == "/absolute/path", "expand_path leaves absolute paths intact");
    if (home && *home) {
        check(bt::expand_path("~/test") == std::filesystem::path(home) / "test",
              "expand_path expands ~/ to $HOME/");
    }

    // 3. Programmatic override
    bt::set_download_dir("/tmp/swarmbit-test-override", false);
    check(bt::download_dir() == "/tmp/swarmbit-test-override",
          "set_download_dir overrides download_dir");

    // Reset override
    bt::set_download_dir("", false);

    // 4. Environment variable override
    setenv("SWARMBIT_DOWNLOAD_DIR", "/tmp/swarmbit-test-env", 1);
    check(bt::download_dir() == "/tmp/swarmbit-test-env",
          "SWARMBIT_DOWNLOAD_DIR overrides download_dir");
    unsetenv("SWARMBIT_DOWNLOAD_DIR");

    check(bt::download_dir() == def_dir,
          "download_dir falls back to default after clearing env and override");
}

int main() {
    // Construct a session to exercise the SessionManager wrapper.
    bt::SessionManager session;
    check(session.session().is_valid(), "session_handle is valid after construction");

    test_state_dir();
    test_download_dir();
    test_magnet_parse();
    test_hex_roundtrip();
    test_find_torrent();
    test_remove();
    test_resume_persistence();

    if (failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
