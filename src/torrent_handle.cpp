#include "torrent_handle.hpp"

#include <libtorrent/bdecode.hpp>
#include <libtorrent/error.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

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

Torrent Torrent::add_file(SessionManager& session,
                          const std::string& torrent_path,
                          const std::string& save_path) {
    const fs::path path(torrent_path);

    std::string buffer = read_file(torrent_path);
    libtorrent::add_torrent_params params =
        libtorrent::load_torrent_buffer(
            libtorrent::span<char const>(buffer.data(),
                                          static_cast<std::ptrdiff_t>(buffer.size())));

    if (!save_path.empty()) {
        params.save_path = bt::expand_path(save_path).string();
    } else if (params.save_path.empty()) {
        params.save_path = bt::download_dir().string();
    }
    std::error_code fec;
    fs::create_directories(params.save_path, fec);

    // Sparse mode: allocate file storage lazily as data is downloaded rather
    // than pre-allocating the full file size up front.
    params.storage_mode = libtorrent::storage_mode_sparse;
    params.flags |= libtorrent::torrent_flags::sequential_download;
    params.max_connections = 200;
    if (params.ti && params.ti->num_pieces() > 0) {
        std::error_code fec;
        const fs::path root_path =
            fs::path(params.save_path) / params.ti->name();
        if (!fs::exists(root_path, fec)) {
            const int num_pieces = params.ti->num_pieces();
            const int piece_len = std::max(1, params.ti->piece_length());
            const int window = std::max(
                16, static_cast<int>((2048LL * 1024 * 1024) / piece_len));
            const int limit = std::min(num_pieces, window);
            params.piece_priorities.assign(
                static_cast<std::size_t>(num_pieces), libtorrent::dont_download);
            for (int i = 0; i < limit; ++i) {
                params.piece_priorities[static_cast<std::size_t>(i)] =
                    libtorrent::default_priority;
            }
        }
    }

    if (params.ti) {
        auto existing = session.session().find_torrent(params.ti->info_hashes().v1);
        if (existing.is_valid()) {
            return Torrent(session, existing);
        }
    }

    boost::system::error_code ec;
    libtorrent::torrent_handle handle = session.session().add_torrent(params, ec);
    if (ec) {
        throw std::runtime_error("failed to add torrent: " + ec.message());
    }
    return Torrent(session, handle);
}

Torrent Torrent::add_magnet(SessionManager& session,
                            const std::string& magnet_uri,
                            const std::string& save_path) {
    libtorrent::add_torrent_params params =
        libtorrent::parse_magnet_uri(magnet_uri);
    if (!save_path.empty()) {
        params.save_path = bt::expand_path(save_path).string();
    } else if (params.save_path.empty()) {
        params.save_path = bt::download_dir().string();
    }
    std::error_code fec;
    fs::create_directories(params.save_path, fec);

    auto existing = session.session().find_torrent(params.info_hashes.v1);
    if (existing.is_valid()) {
        return Torrent(session, existing);
    }

    // Sparse mode: allocate file storage lazily as data is downloaded rather
    // than pre-allocating the full file size up front.
    params.storage_mode = libtorrent::storage_mode_sparse;
    params.flags |= libtorrent::torrent_flags::sequential_download;
    params.max_connections = 200;

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
    if (!is_valid()) return false;
    const auto st = status();
    return st.is_seeding || st.is_finished ||
           st.state == libtorrent::torrent_status::seeding ||
           st.state == libtorrent::torrent_status::finished;
}

Torrent::State Torrent::state() const {
    if (!is_valid()) return State::NotAdded;
    const auto st = status();
    switch (st.state) {
        case libtorrent::torrent_status::checking_files:
        case libtorrent::torrent_status::checking_resume_data:
            return State::Checking;
        case libtorrent::torrent_status::downloading_metadata:
            return State::Downloading;
        case libtorrent::torrent_status::downloading:
            if (st.is_seeding || st.is_finished) return State::Completed;
            return State::Downloading;
        case libtorrent::torrent_status::seeding:
            return State::Seeding;
        case libtorrent::torrent_status::finished:
            return State::Completed;
        default:
            break;
    }
    if (st.is_seeding) return State::Seeding;
    if (st.is_finished) return State::Completed;
    return State::Downloading;
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
    // num_complete is the total number of seeders known via tracker scrape.
    // The tracker only reports it after an announce that includes scrape
    // data, so it stays -1 until then. Fall back to num_seeds (peers we are
    // actually connected to that are seeding) when scrape is unavailable.
    const auto st = status();
    if (st.num_complete >= 0) return st.num_complete;
    return st.num_seeds;
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

long long Torrent::total_size() const {
    if (!is_valid()) return 0;
    auto ti = handle_.torrent_file();
    const auto st = status();
    if (ti && !st.save_path.empty()) {
        const auto& storage = ti->layout();
        long long disk_bytes = 0;
        for (auto index : storage.file_range()) {
            const fs::path p =
                fs::path(st.save_path) /
                storage.file_path(static_cast<libtorrent::file_index_t>(index));
            struct stat sb{};
            if (::stat(p.c_str(), &sb) == 0) {
                disk_bytes += static_cast<long long>(sb.st_blocks) * 512LL;
            }
        }
        if (disk_bytes > 0) {
            return disk_bytes;
        }
    }
    return static_cast<long long>(st.total_done);
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
    libtorrent::remove_flags_t flags{};
    if (delete_files) {
        // Ask libtorrent to delete the downloaded payload (and its partfile)
        // from disk once the torrent is removed from the session.
        flags = libtorrent::session_handle::delete_files;
    }
    session_.session().remove_torrent(handle_, flags);
}

void Torrent::update_sequential_window() {
    if (!is_valid()) return;
    auto ti = handle_.torrent_file();
    if (!ti || ti->num_pieces() <= 0) return;

    const int num_pieces = ti->num_pieces();
    const auto st = status();
    if (st.state == libtorrent::torrent_status::checking_files ||
        st.state == libtorrent::torrent_status::checking_resume_data ||
        st.state == libtorrent::torrent_status::downloading_metadata) {
        return;
    }

    const auto& have = st.pieces;
    if (static_cast<int>(have.size()) != num_pieces) {
        return;
    }

    const int piece_len = std::max(1, ti->piece_length());
    const int window = std::max(
        16, static_cast<int>((2048LL * 1024 * 1024) / piece_len));

    std::vector<libtorrent::download_priority_t> prios(
        static_cast<std::size_t>(num_pieces), libtorrent::dont_download);
    int missing_wanted = 0;
    for (int i = 0; i < num_pieces; ++i) {
        if (have[static_cast<libtorrent::piece_index_t>(i)]) {
            prios[static_cast<std::size_t>(i)] = libtorrent::default_priority;
        } else if (missing_wanted < window) {
            prios[static_cast<std::size_t>(i)] = libtorrent::default_priority;
            ++missing_wanted;
        }
    }

    auto cur = handle_.get_piece_priorities();
    if (cur != prios) {
        handle_.prioritize_pieces(prios);
    }
}

long long Torrent::bytes_remaining() const {
    if (!is_valid()) return 0;
    const auto st = status();
    const long long remaining = st.total_wanted - st.total_wanted_done;
    return remaining > 0 ? remaining : 0;
}

long long Torrent::eta_seconds() const {
    if (!is_valid()) return -1;
    const auto st = status();
    const long long remaining = st.total_wanted - st.total_wanted_done;
    if (st.is_seeding || st.is_finished) return 0;
    // total_wanted is not known until metadata arrives.
    if (!st.has_metadata) return -1;
    if (remaining <= 0) return 0;
    if (st.state == libtorrent::torrent_status::checking_files ||
        st.state == libtorrent::torrent_status::checking_resume_data) {
        return -1;
    }
    if (st.flags & libtorrent::torrent_flags::paused) return -1;
    const long long rate = st.download_payload_rate;
    if (rate <= 0) return -1;
    return (remaining + rate - 1) / rate;  // round up
}

double Torrent::progress() const {
    if (!is_valid()) return -1.0;
    const auto st = status();
    if (st.is_seeding || st.is_finished ||
        st.state == libtorrent::torrent_status::seeding ||
        st.state == libtorrent::torrent_status::finished) {
        return 1.0;
    }
    if (st.total_wanted > 0) {
        if (st.total_wanted_done >= st.total_wanted) return 1.0;
        double p = static_cast<double>(st.total_wanted_done) /
                   static_cast<double>(st.total_wanted);
        return p > 1.0 ? 1.0 : (p < 0.0 ? 0.0 : p);
    }
    auto ti = handle_.torrent_file();
    if (ti && ti->total_size() > 0) {
        if (st.total_done >= ti->total_size()) return 1.0;
        double p = static_cast<double>(st.total_done) /
                   static_cast<double>(ti->total_size());
        return p > 1.0 ? 1.0 : (p < 0.0 ? 0.0 : p);
    }
    double p = static_cast<double>(st.progress);
    return p > 1.0 ? 1.0 : (p < 0.0 ? 0.0 : p);
}

libtorrent::torrent_status Torrent::status() const {
    return handle_.status(
        libtorrent::torrent_handle::query_name |
        libtorrent::torrent_handle::query_save_path |
        libtorrent::torrent_handle::query_pieces |
        libtorrent::torrent_handle::query_accurate_download_counters);
}

std::vector<Torrent::FileInfo> Torrent::files() const {
    std::vector<FileInfo> out;
    auto ti = handle_.torrent_file();
    if (!ti) return out;

    const auto& storage = ti->layout();
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

std::vector<Torrent::PeerInfo> Torrent::peers() const {
    std::vector<PeerInfo> out;
    if (!is_valid()) return out;

    std::vector<libtorrent::peer_info> raw_peers;
    handle_.get_peer_info(raw_peers);
    std::stable_sort(
        raw_peers.begin(), raw_peers.end(),
        [](const libtorrent::peer_info& a, const libtorrent::peer_info& b) {
            if (a.payload_down_speed != b.payload_down_speed) {
                return a.payload_down_speed > b.payload_down_speed;
            }
            return a.payload_up_speed > b.payload_up_speed;
        });
    const std::size_t max_peers = std::min<std::size_t>(raw_peers.size(), 20);
    out.reserve(max_peers);

    for (std::size_t idx = 0; idx < max_peers; ++idx) {
        const auto& p = raw_peers[idx];
        PeerInfo pi;
        if (p.flags & libtorrent::peer_info::i2p_socket) {
            pi.endpoint = "i2p";
        } else {
            const auto ep = p.remote_endpoint();
            pi.endpoint = ep.address().to_string() + ":" + std::to_string(ep.port());
        }
        pi.client = p.client.empty() ? "-" : p.client;
        pi.progress = static_cast<double>(p.progress);
        pi.down_rate = static_cast<long long>(p.payload_down_speed);
        pi.up_rate = static_cast<long long>(p.payload_up_speed);

        if (p.flags & libtorrent::peer_info::connecting) {
            pi.status = "Connecting";
        } else if (p.flags & libtorrent::peer_info::handshake) {
            pi.status = "Handshake";
        } else {
            std::vector<std::string> tags;
            if (p.flags & libtorrent::peer_info::seed) {
                tags.push_back("Seed");
            }
            if (p.flags & libtorrent::peer_info::remote_choked) {
                tags.push_back("Choked");
            } else {
                tags.push_back("Unchoked");
            }
            if (p.flags & libtorrent::peer_info::snubbed) {
                tags.push_back("Snubbed");
            }
            if (p.flags & libtorrent::peer_info::on_parole) {
                tags.push_back("Parole");
            }
            if (p.flags & libtorrent::peer_info::utp_socket) {
                tags.push_back("uTP");
            }
            if (p.flags & libtorrent::peer_info::ssl_socket) {
                tags.push_back("SSL");
            }
            pi.status = "Connected";
            if (!tags.empty()) {
                pi.status += " (";
                for (std::size_t i = 0; i < tags.size(); ++i) {
                    if (i > 0) pi.status += ", ";
                    pi.status += tags[i];
                }
                pi.status += ")";
            }
        }
        out.push_back(std::move(pi));
    }
    return out;
}

}  // namespace bt
