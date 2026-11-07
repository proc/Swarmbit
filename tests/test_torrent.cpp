// Minimal self-contained checks for the torrent client library.
// These do not require network access: they validate magnet-link parsing and
// info-hash hex helpers, which are pure functions.

#include "session_manager.hpp"
#include "torrent_handle.hpp"

#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session_handle.hpp>

#include <chrono>
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
}

}  // namespace

int main() {
    // Construct a session to exercise the SessionManager wrapper.
    bt::SessionManager session;
    check(session.session().is_valid(), "session_handle is valid after construction");

    test_magnet_parse();
    test_hex_roundtrip();
    test_find_torrent();

    if (failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
