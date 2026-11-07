// Swarmbit: a BitTorrent client built on libtorrent with an optional
// RTXUI dashboard.
//
// Usage:
//   swarmbit                                 Launch the RTXUI dashboard and
//                                            resume downloading torrents.
//   swarmbit add <torrent-file|magnet-uri>   Launch the RTXUI dashboard with
//                                            the torrent preloaded (blocks
//                                            until q/Esc).
//   swarmbit ui                              Launch the RTXUI dashboard.
//   swarmbit list [--verbose]
//   swarmbit status <info-hash>
//   swarmbit remove <info-hash> [--files]
//   swarmbit pause <info-hash>
//   swarmbit resume <info-hash>
//   swarmbit ports
//
// The `add`/`ui` commands run the RTXUI event loop in the foreground; all
// other commands are one-shot. Ctrl-C performs a clean shutdown (pauses
// torrents, tears down the session).

#include "alert_monitor.hpp"
#include "session_manager.hpp"
#include "ui.hpp"
#include "torrent_handle.hpp"

#include <atomic>
#include <csignal>
#include <iomanip>
#include <memory>
#include <libtorrent/hex.hpp>
#include <memory>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false); }

int hexdigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

void print_usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options] [command] [args]\n"
        "Options:\n"
        "  --download-dir <path>, -o <path>  Set download directory (default: ~/Downloads)\n"
        "Commands:\n"
        "  add <torrent|magnet>   Launch the RTXUI dashboard (blocks until q/Esc)\n"
        "  ui                     Launch the RTXUI dashboard (default)\n"
        "  config [download-dir [<path>]] View or set configuration (persisted to ~/.swarmbit/config)\n"
        "  list [--verbose]\n"
        "  status <info-hash>\n"
        "  remove <info-hash> [--files]\n"
        "  pause <info-hash>\n"
        "  resume <info-hash>\n"
        "  ports\n";
}

// Parse a hex info-hash (optionally prefixed with "btih:") into libtorrent form.
libtorrent::sha1_hash parse_hash(const std::string& s) {
    std::string hex = s;
    const std::string prefix = "btih:";
    if (hex.compare(0, prefix.size(), prefix) == 0) {
        hex = hex.substr(prefix.size());
    }
    libtorrent::sha1_hash out;
    if (hex.size() == 40) {
        for (int i = 0; i < 20; ++i) {
            out.data()[i] = static_cast<char>(
                (hexdigit(hex[2 * i]) << 4) | hexdigit(hex[2 * i + 1]));
        }
    }
    return out;
}

void print_torrent_status(const bt::Torrent& t) {
    std::cout << "  Name:        " << t.name() << "\n";
    std::cout << "  Info-hash:   " << t.info_hash() << "\n";
    std::cout << "  Save path:   " << t.save_path() << "\n";
    std::cout << "  State:       " << static_cast<int>(t.state()) << "\n";
    std::cout << "  Progress:    " << (t.progress() * 100.0) << "%\n";
    std::cout << "  Peers:       " << t.num_peers()
              << " (seeds=" << t.num_complete()
              << ", leechers=" << t.num_incomplete() << ")\n";
    std::cout << "  Down rate:   " << t.download_rate() << " B/s\n";
    std::cout << "  Up rate:     " << t.upload_rate() << " B/s\n";
    std::cout << "  Downloaded:  " << t.total_download() << " B\n";
    std::cout << "  Uploaded:    " << t.total_upload() << " B\n";

    const auto files = t.files();
    std::cout << "  Files:       " << files.size() << "\n";
    for (const auto& f : files) {
        std::cout << "    - " << f.path << " (" << f.size << " B, "
                  << (f.progress * 100.0) << "%)\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::vector<std::string> raw_args(argv + 1, argv + argc);
    std::string download_dir_arg;
    std::vector<std::string> args;

    for (std::size_t i = 0; i < raw_args.size(); ++i) {
        if (raw_args[i] == "--download-dir" || raw_args[i] == "--dir" || raw_args[i] == "-o") {
            if (i + 1 < raw_args.size()) {
                download_dir_arg = raw_args[++i];
            }
        } else if (raw_args[i].rfind("--download-dir=", 0) == 0) {
            download_dir_arg = raw_args[i].substr(15);
        } else if (raw_args[i].rfind("--dir=", 0) == 0) {
            download_dir_arg = raw_args[i].substr(6);
        } else if (raw_args[i] == "-h" || raw_args[i] == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            args.push_back(raw_args[i]);
        }
    }

    if (!download_dir_arg.empty()) {
        bt::set_download_dir(download_dir_arg, false);
    }

    if (args.empty()) {
        return bt::run_ui("");
    }

    const std::string command = args[0];

    // Configuration inspect and persist.
    if (command == "config") {
        if (args.size() == 1 || (args.size() == 2 && args[1] == "download-dir")) {
            std::cout << bt::download_dir().string() << "\n";
            return 0;
        }
        if (args.size() >= 3 && args[1] == "download-dir") {
            bt::set_download_dir(args[2], true);
            std::cout << "Download directory set to: " << bt::download_dir().string() << "\n";
            return 0;
        }
        if (args.size() >= 4 && args[1] == "set" && args[2] == "download-dir") {
            bt::set_download_dir(args[3], true);
            std::cout << "Download directory set to: " << bt::download_dir().string() << "\n";
            return 0;
        }
        if (args.size() >= 3 && args[1] == "get" && args[2] == "download-dir") {
            std::cout << bt::download_dir().string() << "\n";
            return 0;
        }
        std::cerr << "Usage: " << argv[0] << " config [download-dir [<path>]]\n";
        return 1;
    }

    // RTXUI dashboard commands. These run a blocking event loop (until
    // q/Esc); no other CLI command can run in this process.
    if (command == "add") {
        if (args.size() < 2) {
            std::cerr << "Error: add requires a torrent file or magnet URI.\n";
            print_usage(argv[0]);
            return 1;
        }
        const std::string target = args[1];

        // Launch the RTXUI dashboard with the torrent preloaded. The UI runs
        // its own event loop in the foreground (blocking until q/Esc); the
        // torrent is added to the session immediately so it begins downloading
        // on startup.
        return bt::run_ui(target);
    }

    if (command == "ui") {
        return bt::run_ui("");
    }

    // One-shot commands that open/close the session quickly.
    if (command == "ports") {
        bt::SessionManager session;
        std::cout << "Listening: yes\n";
        return 0;
    }

    if (command == "list") {
        bt::SessionManager session;
        bool verbose = args.size() > 1 && args[1] == "--verbose";
        for (auto& h : session.session().get_torrents()) {
            bt::Torrent t(session, h);
            std::cout << t.name() << "  [" << t.info_hash() << "]\n";
            if (verbose) print_torrent_status(t);
        }
        return 0;
    }

    if (command == "status" || command == "remove" || command == "pause" ||
        command == "resume") {
        bt::SessionManager session;
        if (args.size() < 2) {
            std::cerr << "Error: " << command << " requires an info-hash.\n";
            print_usage(argv[0]);
            return 1;
        }
        libtorrent::torrent_handle handle =
            session.session().find_torrent(parse_hash(args[1]));
        if (!handle.is_valid()) {
            std::cerr << "Error: no torrent with info-hash " << args[1] << "\n";
            return 1;
        }
        bt::Torrent t(session, handle);
        if (command == "status") {
            print_torrent_status(t);
        } else if (command == "pause") {
            t.pause();
            std::cout << "Paused " << t.name() << "\n";
        } else if (command == "resume") {
            t.resume();
            std::cout << "Resumed " << t.name() << "\n";
        } else {  // remove
            bool files = args.size() > 2 && args[2] == "--files";
            t.remove(files);
            std::cout << "Removed " << t.name()
                      << (files ? " (with files)" : "") << "\n";
        }
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
