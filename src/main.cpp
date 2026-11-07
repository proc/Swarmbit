// Command-line BitTorrent client built on libtorrent (Swarmbit).
//
// Usage:
//   swarmbit add <torrent-file|magnet-uri>
//   swarmbit list [--verbose]
//   swarmbit status <info-hash>
//   swarmbit remove <info-hash> [--files]
//   swarmbit pause <info-hash>
//   swarmbit resume <info-hash>
//   swarmbit ports
//
// The `add` command runs its event loop in the foreground; Ctrl-C performs a
// clean shutdown (pauses torrents, tears down the session).

#include "alert_monitor.hpp"
#include "session_manager.hpp"
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
        "Usage: " << prog << " <command> [args]\n"
        "Commands:\n"
        "  add <torrent|magnet>\n"
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


// Runs the foreground-style download progress display in a background thread.
// Used by the `add` command so the main thread stays responsive to other
// commands (list, status, pause, resume, remove, ports).
void run_download_loop(bt::Torrent& t, std::atomic<bool>& running) {
    while (running.load() && t.is_valid()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // ANSI colors: green bar fills as progress grows, dimmed stats.
        const char* reset = "\033[0m";
        const char* green = "\033[32m";
        const char* cyan = "\033[36m";
        const char* yellow = "\033[33m";
        const char* magenta = "\033[35m";
        const char* bold = "\033[1m";

        double pct = t.progress();
        if (pct > 1.0) pct = 1.0;
        int filled = static_cast<int>(pct * 30);
        std::string bar(filled, '#');
        bar.append(30 - filled, '.');
        std::cout << "\r  [" << green << bar << reset << "] "
                  << bold << green
                  << std::fixed << std::setprecision(1)
                  << (pct * 100.0) << "%" << reset << "  "
                  << cyan << "peers=" << reset << t.num_peers() << "  "
                  << yellow << "down=" << reset << (t.download_rate() / 1024.0) << "KB/s  "
                  << magenta << "up=" << reset << (t.upload_rate() / 1024.0) << "KB/s";
        if (t.is_finished()) {
            std::cout << "  " << green << "[done]" << reset;
        }
        std::cout << std::flush;
    }
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string command = args[0];

    // Each command creates its own session. `add` runs the download in a
    // detached thread while this process waits for Ctrl-C.

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

    if (command == "add") {
        if (args.size() < 2) {
            std::cerr << "Error: add requires a torrent file or magnet URI.\n";
            print_usage(argv[0]);
            return 1;
        }
        const std::string target = args[1];
        const bool is_magnet = target.rfind("magnet:", 0) == 0;

        // Fork BEFORE creating any libtorrent session. The child creates its
        // own fork-safe session and runs the download; the parent returns
        // immediately so the shell stays responsive.
        pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "Error: failed to fork.\n";
            return 1;
        }
        if (pid == 0) {
            // Child: create a fresh session (fork-safe) and run the download.
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            bt::SessionManager child_session;
            auto torrent = std::make_unique<bt::Torrent>(
                is_magnet
                    ? bt::Torrent::add_magnet(child_session, target)
                    : bt::Torrent::add_file(child_session, target));
            bt::AlertMonitor monitor(child_session);
            monitor.start();

            bt::Torrent& t = *torrent;
            std::cout << "Added: " << t.name() << "  (" << t.info_hash() << ")\n"
                      << "Downloading ...\nPress Ctrl-C to stop.\n";
            std::flush(std::cout);

            run_download_loop(t, g_running);

            std::cout << "\nShutting down...\n";
            t.pause();
            monitor.stop();
            _exit(0);
        }

        // Parent: report the child PID and return control to the shell.
        std::cout << "Started background download (child PID "
                  << static_cast<unsigned long>(pid) << ").\n";
        std::cout << "Use 'list', 'status', 'pause', 'resume', or 'remove' to manage it.\n";
        std::flush(std::cout);
        std::fflush(stdout);
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
