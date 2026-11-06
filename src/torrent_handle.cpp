#include "torrent_handle.hpp"

#include <libtorrent/error.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_info.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace bt {
namespace fs = std::filesystem;

namespace {

// Local hex encoder: avoids libtorrent::aux::to_hex, whose overloads are
// hidden (non-external) in the Homebrew libtorrent .dylib and fail to link.
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

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

Torrent::Torrent(SessionManager& session, libtorrent::torrent_handle handle)
    : session_(session), handle_(std::move(handle)) {}

Torrent Torrent::add_file(SessionManager& session, const std::string& torrent_path) {
    const fs::path path(torrent_path);

    std::string buffer = read_file(torrent_path);
    libtorrent::span<char const> buf_span(buffer.data(),
                                          static_cast<std::ptrdiff_t>(buffer.size()));
    libtorrent::torrent_info info(buf_span, libtorrent::from_span_t{});

    libtorrent::add_torrent_params params;
    params.ti = std::make_shared<libtorrent::torrent_info>(info);
    params.info_hashes = info.info_hashes();

    if (params.save_path.empty()) {
        std::error_code fec;
        params.save_path = fs::current_path(fec).string();
    }

    boost::system::error_code ec;
    libtorrent::torrent_handle handle = session.session().add_torrent(params, ec);
    if (ec) {
        throw std::runtime_error("failed to add torrent: " + ec.message());
    }
    return Torrent(session, handle);
}

Torrent Torrent::add_magnet(SessionManager& session, const std::string& magnet_uri) {
    libtorrent::add_torrent_params params =
        libtorrent::parse_magnet_uri(magnet_uri);
    if (params.save_path.empty()) {
        std::error_code fec;
        params.save_path = fs::current_path(fec).string();
    }
    libtorrent::torrent_handle handle =
        session.session().add_torrent(params);
    return Torrent(session, handle);
}

const libtorrent::torrent_handle& Torrent::handle() const {
    return handle_;
}

bool Torrent::is_valid() const {
    return handle_.is_valid();
}

bool Torrent::is_finished() const {
    return status().is_finished;
}

Torrent::State Torrent::state() const {
    if (!is_valid()) return State::NotAdded;
    switch (status().state) {
        case libtorrent::torrent_status::checking_files:
        case libtorrent::torrent_status::checking_resume_data:
            return State::Checking;
        case libtorrent::torrent_status::downloading_metadata:
        case libtorrent::torrent_status::downloading:
            return State::Downloading;
        case libtorrent::torrent_status::seeding:
            return State::Seeding;
        case libtorrent::torrent_status::finished:
            return State::Completed;
        default:
            return State::Downloading;
    }
}

long long Torrent::download_rate() const {
    return status().download_rate;
}

long long Torrent::upload_rate() const {
    return status().upload_rate;
}

int Torrent::num_peers() const {
    return status().num_peers;
}

int Torrent::num_complete() const {
    return status().num_complete;
}

int Torrent::num_incomplete() const {
    return status().num_incomplete;
}

long long Torrent::total_download() const {
    return status().total_download;
}

long long Torrent::total_upload() const {
    return status().total_upload;
}

std::string Torrent::name() const {
    return status().name;
}

std::string Torrent::save_path() const {
    return status().save_path;
}

std::string Torrent::info_hash() const {
    if (!is_valid()) return "";
    const libtorrent::sha1_hash h =
        handle_.status().info_hashes.v1;
    return to_hex(h.data(), 20);
}

void Torrent::pause() {
    if (is_valid()) handle_.pause();
}

void Torrent::resume() {
    if (is_valid()) handle_.resume();
}

void Torrent::remove(bool delete_files) {
    if (!is_valid()) return;
    libtorrent::remove_flags_t flags;
    if (delete_files) {
        flags = libtorrent::remove_flags_t(0);
    }
    session_.session().remove_torrent(handle_, flags);
}

double Torrent::progress() const {
    if (!is_valid()) return -1.0;
    return static_cast<double>(status().progress);
}

libtorrent::torrent_status Torrent::status() const {
    return handle_.status(
        libtorrent::torrent_handle::query_name |
        libtorrent::torrent_handle::query_save_path |
        libtorrent::torrent_handle::query_pieces);
}

std::vector<Torrent::FileInfo> Torrent::files() const {
    std::vector<FileInfo> out;
    auto ti = handle_.torrent_file();
    if (!ti) return out;

    const auto& storage = ti->files();
    const int num_pieces = ti->num_pieces();
    const double piece_size = static_cast<double>(storage.piece_length());

    const double have_fraction =
        num_pieces > 0
            ? static_cast<double>(status().pieces.count()) / num_pieces
            : 0.0;

    for (auto index : storage.file_range()) {
        FileInfo fi;
        fi.path = storage.file_path(static_cast<libtorrent::file_index_t>(index));

        // map_file(index, 0, 0) returns the file's offset and size.
        const auto pr = storage.map_file(
            static_cast<libtorrent::file_index_t>(index), 0, 0);
        const std::int64_t offset = pr.start;
        fi.size = static_cast<long long>(pr.length);

        if (piece_size <= 0 || num_pieces == 0) {
            fi.progress = have_fraction;
            out.push_back(std::move(fi));
            continue;
        }

        // Map the file's byte range onto the pieces it spans and count how
        // many of those pieces are fully present.
        const int first_piece = static_cast<int>(offset / piece_size);
        const int last_piece = std::min<int>(
            static_cast<int>((offset + pr.length + piece_size - 1) / piece_size),
            num_pieces);
        if (first_piece >= last_piece) {
            fi.progress = have_fraction;
            out.push_back(std::move(fi));
            continue;
        }

        const auto& pieces = status().pieces;
        int have = 0;
        for (int p = first_piece; p < last_piece; ++p) {
            if (p < num_pieces && pieces[static_cast<libtorrent::piece_index_t>(p)]) {
                ++have;
            }
        }
        fi.progress = static_cast<double>(have) /
                      static_cast<double>(last_piece - first_piece);
        out.push_back(std::move(fi));
    }
    return out;
}

}  // namespace bt
