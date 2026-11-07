// Fullscreen RTXUI dashboard for Swarmbit, a BitTorrent client built on
// libtorrent. Drives the same SessionManager/Torrent layer as the CLI, but
// presents an interactive, reactive terminal UI instead of subcommands.
//
// Layout: a header, an "add" prompt at the top, and a scrollable list of
// torrents. Selecting a torrent shows its progress bar and stats. Keyboard:
//   j / Down   select next     k / Up   select previous
//   g          go to top       G        go to bottom
//   Enter      add             d      download/pause toggle
//   u          resume          x      remove (keep files)
//   X          remove and delete downloaded files
//   q / Esc    quit
//
// The torrent list is polled from a background task every 400 ms so the UI
// stays live while downloads proceed. RTXUI re-renders only changed cells.

#include "alert_monitor.hpp"
#include "session_manager.hpp"
#include "torrent_handle.hpp"
#include "ui.hpp"

#include <libtorrent/announce_entry.hpp>
#include <libtorrent/hex.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <rtxui/rtxui.hpp>

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ui {
// Row, PeerRow, and Fields() are declared in ui.hpp.

void PeerFields(const PeerRow& p, rtxui::FieldSink& out) {
    out("endpoint", p.endpoint);
    out("client", p.client);
    out("status", p.status);
    out("progress_pct", p.progress_pct);
    out("down_rate_str", p.down_rate_str);
    out("up_rate_str", p.up_rate_str);
}

void RowFields(const Row& r, rtxui::FieldSink& out) {
    out("info_hash", r.info_hash);
    out("name", r.name);
    out("state", r.state);
    out("progress", std::to_string(r.progress));
    out("size", std::to_string(r.size));
    out("downloaded", std::to_string(r.downloaded));
    out("upload_total", std::to_string(r.upload_total));
    out("peers", std::to_string(r.peers));
    out("seeds", std::to_string(r.seeds));
    out("down_rate", std::to_string(r.down_rate));
    out("up_rate", std::to_string(r.up_rate));
    out("finished", r.finished ? "finished" : "downloading");
    // Pre-computed display fields. RTXUI templates cannot evaluate
    // expressions ({r.progress * 100} renders literally), so the template
    // reads these plain fields instead.
    out("selected_class", r.selected_class);
    out("progress_pct_num", std::to_string(r.progress_pct_num));
    out("progress_pct", r.progress_pct);
    out("downloaded_str", r.downloaded_str);
    out("upload_total_str", r.upload_total_str);
    out("down_rate_str", r.down_rate_str);
    out("up_rate_str", r.up_rate_str);
    out("detail_progress", r.detail_progress);
    out("detail_progress_num", std::to_string(r.detail_progress_num));
    out("detail_size", r.detail_size);
    out("detail_downloaded", r.detail_downloaded);
    out("detail_uploaded", r.detail_uploaded);
    out("detail_down_rate", r.detail_down_rate);
    out("detail_up_rate", r.detail_up_rate);
    out("detail_eta", r.detail_eta);
    out("detail_bar_width", std::to_string(r.detail_bar_width));
}

void Fields(const Row& r, rtxui::FieldSink& out) {
    RowFields(r, out);
}

}  // namespace ui

using namespace rtxui;

namespace {

int hexdigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// Human-readable bytes (SI, base-1024).
std::string human_bytes(long long bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    if (bytes == 0) return "0 B";
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    char buf[32];
    if (i == 0) {
        std::snprintf(buf, sizeof(buf), "%lld %s", bytes, units[i]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[i]);
    }
    return buf;
}

// Parse the hex info-hash shown in the UI back into libtorrent form so the
// pause/resume/remove actions can find the torrent in a fresh session.
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
                ((hexdigit(hex[2 * i]) << 4) | hexdigit(hex[2 * i + 1])) & 0xff);
        }
    }
    return out;
}

std::string human_rate(long long bytes_per_sec) {
    return human_bytes(bytes_per_sec) + "/s";
}

// Human-readable ETA from Torrent::eta_seconds(): "Done" for 0, "∞" when
// unknown (-1), otherwise the two most significant units, e.g. "2d 03h",
// "1h 05m", "4m 09s", "37s".
std::string human_eta(long long secs) {
    if (secs < 0) return "\u221E";
    if (secs == 0) return "Done";
    const long long days = secs / 86400;
    const long long hours = (secs % 86400) / 3600;
    const long long mins = (secs % 3600) / 60;
    const long long s = secs % 60;
    char buf[32];
    if (days >= 100) {
        return "\u221E";  // too far out to be meaningful
    } else if (days > 0) {
        std::snprintf(buf, sizeof(buf), "%lldd %02lldh", days, hours);
    } else if (hours > 0) {
        std::snprintf(buf, sizeof(buf), "%lldh %02lldm", hours, mins);
    } else if (mins > 0) {
        std::snprintf(buf, sizeof(buf), "%lldm %02llds", mins, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%llds", s);
    }
    return buf;
}

// Detect the terminal width in columns, falling back gracefully if not a tty.
int terminal_columns() {
    const char* headless = std::getenv("RTXUI_HEADLESS");
    if (headless) {
        int w = 0, h = 0;
        if (std::sscanf(headless, "%dx%d", &w, &h) == 2 && w > 0) return w;
    }
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    const char* cols = std::getenv("COLUMNS");
    if (cols) {
        int c = std::atoi(cols);
        if (c > 0) return c;
    }
    return 100;
}

// Polls the live session into a vector of Rows. The UI holds one of these and
// refreshes it on a timer.
//
// The Snapshot owns a single persistent SessionManager so that torrents added
// through the UI's "add" field live in the same session that polls them —
// each SessionManager is its own libtorrent session, so a per-poll session
// would never see what Add() inserted.
class Snapshot {
public:
    explicit Snapshot()
        : session_(std::make_unique<bt::SessionManager>(resume_dir_string())) {
        // Restore from fast-resume data first so torrents come back with
        // their progress (no full recheck) and their saved paused/running
        // state. load_persisted() then adds anything that has no resume file
        // yet (e.g. added just before a crash).
        session_->restore_torrents();
        load_persisted();
    }

    bt::SessionManager& session() { return *session_; }

    // Where per-torrent resume data is persisted so completed/in-progress
    // torrents reappear after a restart. Used by run_ui() to configure the
    // AlertMonitor's resume dir so it matches restore_torrents().
    std::string resume_dir_string() const {
        return resume_dir_string_static();
    }

    void refresh() {
        std::lock_guard<std::mutex> lock(mutex_);
        rows_.ptr->clear();
        *rows_.ptr = gather();
    }

    static std::string resume_dir_string_static() {
        std::error_code ec;
        return (state_dir() / "resume").string();
    }

    // Add a torrent file or magnet URI to the owning session.
    void add(const std::string& target) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool is_magnet = target.rfind("magnet:", 0) == 0;
        std::string resolved_target = target;
        if (!is_magnet) {
            std::error_code ec;
            auto abs = std::filesystem::absolute(target, ec);
            if (!ec) resolved_target = abs.string();
        }
        bt::Torrent t = is_magnet
                            ? bt::Torrent::add_magnet(*session_, resolved_target)
                            : bt::Torrent::add_file(*session_, resolved_target);
        t.resume();

        const std::string hash = t.info_hash();
        if (!hash.empty()) {
            if (!is_magnet) {
                cache_torrent_file_from_path(hash, resolved_target);
            }
            persisted_[hash] = PersistedEntry{
                hash, t.save_path(), resolved_target, true};
            save_persisted();
        }
    }

    // Drive the torrent whose hash is `hash`: 0=toggle pause, 1=resume,
    // 2=remove with files, 3=remove keeping files.
    void control(const std::string& hash, int action) {
        std::lock_guard<std::mutex> lock(mutex_);
        libtorrent::sha1_hash h = parse_hash(hash);
        auto handle = session_->session().find_torrent(h);
        if (!handle.is_valid()) return;
        bt::Torrent t(*session_, handle);
        switch (action) {
            case 0:  // toggle download/pause
                if (static_cast<bool>(handle.flags() & libtorrent::torrent_flags::paused)) {
                    t.resume();
                    auto it = persisted_.find(hash);
                    if (it != persisted_.end()) {
                        it->second.active = true;
                        save_persisted();
                    }
                } else {
                    t.pause();
                    auto it = persisted_.find(hash);
                    if (it != persisted_.end()) {
                        it->second.active = false;
                        save_persisted();
                    }
                }
                break;
            case 1:  // resume
                t.resume();
                {
                    auto it = persisted_.find(hash);
                    if (it != persisted_.end()) {
                        it->second.active = true;
                        save_persisted();
                    }
                }
                break;
            case 2:  // remove (with files)
            case 3:  // remove (keep files)
                t.remove(action == 2);
                persisted_.erase(hash);
                {
                    std::error_code ec;
                    std::filesystem::remove(cached_torrent_path(hash), ec);
                }
                save_persisted();
                break;
        }
    }

private:
    struct PersistedEntry {
        std::string info_hash;
        std::string save_path;
        std::string target;
        bool active = true;
    };

    static std::filesystem::path state_dir() {
        return bt::state_dir();  // ~/.swarmbit
    }

    static std::filesystem::path state_file_path() {
        return state_dir() / "downloading.tsv";
    }

    static std::filesystem::path cached_torrent_path(const std::string& hash) {
        return state_dir() / "torrents" / (hash + ".torrent");
    }

    static void cache_torrent_file_from_path(const std::string& hash,
                                             const std::string& src_path) {
        std::error_code ec;
        const auto dst = cached_torrent_path(hash);
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::filesystem::copy_file(
            src_path, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
    }

    static void cache_torrent_metadata(const std::string& hash,
                                       const libtorrent::torrent_handle& h) {
        const auto dst = cached_torrent_path(hash);
        std::error_code ec;
        if (std::filesystem::exists(dst, ec)) return;
        auto ti = h.torrent_file();
        if (!ti) return;
        try {
            libtorrent::add_torrent_params atp;
            atp.ti = std::const_pointer_cast<libtorrent::torrent_info>(ti);
            for (const auto& tr : h.trackers()) {
                atp.trackers.push_back(tr.url);
                atp.tracker_tiers.push_back(tr.tier);
            }
            std::vector<char> buf = libtorrent::write_torrent_file_buf(
                atp, libtorrent::write_flags::allow_missing_piece_layer);
            std::filesystem::create_directories(dst.parent_path(), ec);
            std::ofstream out(dst, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            }
        } catch (const std::exception&) {
        }
    }

    void save_persisted() {
        std::error_code ec;
        std::filesystem::create_directories(state_dir(), ec);
        std::ofstream out(state_file_path(), std::ios::trunc);
        if (!out) return;
        for (const auto& [hash, entry] : persisted_) {
            if (!entry.active) continue;
            out << entry.info_hash << '\t'
                << entry.save_path << '\t'
                << entry.target << '\n';
        }
    }

    void load_persisted() {
        std::ifstream in(state_file_path());
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            const auto t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const auto t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;

            PersistedEntry entry;
            entry.info_hash = line.substr(0, t1);
            entry.save_path = line.substr(t1 + 1, t2 - (t1 + 1));
            entry.target = line.substr(t2 + 1);
            entry.active = true;
            if (entry.info_hash.empty() || entry.target.empty()) continue;

            // Already restored from resume data: just honour the "active"
            // state recorded here instead of adding it a second time.
            {
                auto existing =
                    session_->session().find_torrent(parse_hash(entry.info_hash));
                if (existing.is_valid()) {
                    bt::Torrent(*session_, existing).resume();
                    persisted_[entry.info_hash] = std::move(entry);
                    continue;
                }
            }

            try {
                const bool is_magnet = entry.target.rfind("magnet:", 0) == 0;
                const auto cached = cached_torrent_path(entry.info_hash);
                std::error_code ec;
                const bool has_cached = std::filesystem::exists(cached, ec);

                if (is_magnet && has_cached) {
                    bt::Torrent t = bt::Torrent::add_file(
                        *session_, cached.string(), entry.save_path);
                    auto mparams = libtorrent::parse_magnet_uri(entry.target);
                    for (const auto& tr : mparams.trackers) {
                        t.handle().add_tracker(libtorrent::announce_entry(tr));
                    }
                    t.resume();
                } else if (is_magnet) {
                    bt::Torrent t = bt::Torrent::add_magnet(
                        *session_, entry.target, entry.save_path);
                    t.resume();
                } else {
                    const std::string path_to_use =
                        has_cached ? cached.string() : entry.target;
                    bt::Torrent t = bt::Torrent::add_file(
                        *session_, path_to_use, entry.save_path);
                    t.resume();
                }
                persisted_[entry.info_hash] = std::move(entry);
            } catch (const std::exception&) {
                // Skip invalid or missing entries without crashing startup.
            }
        }
    }
    std::vector<ui::Row> gather() {
        std::vector<ui::Row> out;
        try {
            for (auto& h : session_->session().get_torrents()) {
                bt::Torrent t(*session_, h);
                if (!t.is_valid()) continue;
                t.update_sequential_window();
                ui::Row r;
                r.info_hash = t.info_hash();
                cache_torrent_metadata(r.info_hash, h);
                if (t.is_finished()) {
                    auto it = persisted_.find(r.info_hash);
                    if (it != persisted_.end() && it->second.active) {
                        it->second.active = false;
                        save_persisted();
                    }
                }
                r.name = t.name();
                r.progress = t.progress();
                r.size = t.total_size();
                r.downloaded = t.total_download();
                r.upload_total = t.total_upload();
                r.peers = t.num_peers();
                r.seeds = t.num_complete();
                r.down_rate = t.download_rate();
                r.up_rate = t.upload_rate();
                r.finished = t.is_finished();
                switch (t.state()) {
                    case bt::Torrent::State::Downloading: r.state = "Downloading"; break;
                    case bt::Torrent::State::Seeding: r.state = "Seeding"; break;
                    case bt::Torrent::State::Completed: r.state = "Completed"; break;
                    case bt::Torrent::State::Checking: r.state = "Checking"; break;
                    default: r.state = "Idle"; break;
                }
                // Pre-compute display strings so the template can use simple
                // field access (RTXUI does not evaluate expressions).
                double pct = r.progress;
                if (r.finished || r.state == "Completed" || r.state == "Seeding") {
                    pct = 1.0;
                }
                if (pct > 1.0) pct = 1.0;
                if (pct < 0.0) pct = 0.0;
                r.progress_pct_num = pct * 100.0;
                {
                    std::ostringstream ss;
                    ss << std::fixed << std::setprecision(1) << (pct * 100.0);
                    r.progress_pct = ss.str() + "%";
                }
                r.downloaded_str = human_bytes(r.downloaded);
                r.upload_total_str = human_bytes(r.upload_total);
                r.down_rate_str = human_rate(r.down_rate);
                r.up_rate_str = human_rate(r.up_rate);
                r.detail_progress_num = pct * 100.0;
                r.detail_progress = r.progress_pct;
                r.detail_size = human_bytes(r.size);
                r.detail_downloaded = r.downloaded_str;
                r.detail_uploaded = r.upload_total_str;
                r.detail_down_rate = r.down_rate_str;
                r.detail_up_rate = r.up_rate_str;
                r.detail_eta = human_eta(t.eta_seconds());
                r.detail_bar_width = std::max(10, terminal_columns() - 6);
                for (const auto& pi : t.peers()) {
                    auto single_line = [](std::string s, std::size_t max_len) {
                        for (char& ch : s) {
                            if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
                        }
                        if (s.size() > max_len) {
                            s.resize(max_len);
                        }
                        return s;
                    };
                    ui::PeerRow pr;
                    pr.endpoint = single_line(pi.endpoint, 32);
                    pr.client = single_line(pi.client, 20);
                    pr.status = single_line(pi.status, 34);
                    double ppct = pi.progress;
                    if (ppct > 1.0) ppct = 1.0;
                    if (ppct < 0.0) ppct = 0.0;
                    {
                        std::ostringstream pss;
                        pss << std::fixed << std::setprecision(1) << (ppct * 100.0);
                        pr.progress_pct = pss.str() + "%";
                    }
                    pr.down_rate_str = human_rate(pi.down_rate);
                    pr.up_rate_str = human_rate(pi.up_rate);
                    r.peer_list.push_back(std::move(pr));
                }
                out.push_back(std::move(r));
            }
        } catch (const std::exception& e) {
            // A transient session hiccup should not crash the UI; keep the
            // previous snapshot.
        }
        return out;
    }

public:
    // Copy of the latest gathered rows (thread-safe behind mutex_).
    std::vector<ui::Row> snapshot_rows() {
        std::lock_guard<std::mutex> lock(mutex_);
        return *rows_.ptr;
    }

    // A fresh RowList with a new shared_ptr to a copy of the current rows.
    // RTXUI's TypeErasedRangeImpl compares shared_ptrs to detect changes, so
    // returning the same shared_ptr would make it think nothing changed.
    ui::RowList get_rows() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto copy = std::make_shared<std::vector<ui::Row>>(*rows_.ptr);
        return ui::RowList(copy);
    }

private:
    std::unique_ptr<bt::SessionManager> session_;
    std::mutex mutex_;
    ui::RowList rows_;
    std::map<std::string, PersistedEntry> persisted_;
};
}  // namespace


// The interactive dashboard component.
class SwarmbitUI : public Component<SwarmbitUI> {
public:
    ui::RowList rows;
    ui::RowList detail;
    ui::PeerList selected_peers;
    bool has_detail = false;
    bool has_peers = false;
    bool no_peers = true;
    std::size_t selected = 0;
    std::string status_msg;
    int term_width_ = 0;
    rtxui::Screen* screen_ = nullptr;  // set in main() for quit (public so main can set it)

    int detail_bar_width() const {
        int w = term_width_ > 0 ? term_width_ : terminal_columns();
        return std::max(10, w - 6);
    }

    // Global keyboard shortcuts. Returns true if the key was handled so it
    // doesn't fall through to focused widgets.
    bool OnEvent(rtxui::Event event) override {
        if (event.is<rtxui::Event::Resized>()) {
            const auto& r = event.get<rtxui::Event::Resized>();
            term_width_ = r.width();
            refresh_detail();
            return ComponentBase::OnEvent(event);
        }
        if (!event.is<rtxui::Event::Keyboard>()) {
            return ComponentBase::OnEvent(event);
        }
        const auto& key = event.get<rtxui::Event::Keyboard>();
        const auto spec = key.special();
        const auto mod = key.modifier();
        const bool ctrl = mod.ctrl();

        // Arrow-key navigation.
        if (spec == rtxui::Event::Keyboard::ArrowDown) {
            Move(1);
            return true;
        }
        if (spec == rtxui::Event::Keyboard::ArrowUp) {
            Move(-1);
            return true;
        }
        if (spec == rtxui::Event::Keyboard::PageDown) {
            Move(5);
            return true;
        }
        if (spec == rtxui::Event::Keyboard::PageUp) {
            Move(-5);
            return true;
        }
        if (spec == rtxui::Event::Keyboard::Home) {
            Top();
            return true;
        }
        if (spec == rtxui::Event::Keyboard::End) {
            Bottom();
            return true;
        }

        // Single-character shortcuts, only when no modifier is held (so typed
        // text into the add field isn't intercepted).
        if (ctrl || mod.alt() || mod.meta()) {
            return ComponentBase::OnEvent(event);
        }
        const char cp = static_cast<char>(key.codepoint());
        if (cp == 'g') {
            Top();
            return true;
        }
        if (cp == 'G') {
            Bottom();
            return true;
        }
        // Shift+X removes the torrent and deletes its downloaded data.
        // Terminals deliver Shift+x as an uppercase 'X' and usually don't set
        // the shift modifier for printable keys, so match the codepoint
        // alone. Lowercase 'x' (below) removes but keeps the data.
        if (cp == 'X') {
            RemoveWithFiles();
            return true;
        }
        const char c = (cp >= 'a' && cp <= 'z') ? cp - 'a' + 'A' : cp;
        switch (c) {
            case 'J': Move(1); return true;
            case 'K': Move(-1); return true;
            case 'D': ToggleDownload(); return true;
            case 'U': Resume(); return true;
            case 'X': Remove(); return true;
            case 'Q':
                if (screen_) {
                    screen_->Exit();
                }
                return true;
        }
        return ComponentBase::OnEvent(event);
    }

    const ui::Row* current() const {
        if (rows.empty()) return nullptr;
        if (selected >= rows.size()) return &rows.back();
        return &rows.at(selected);
    }

    // Rebuild the single-element detail collection for the selected row and
    // tag the selected row with `selected_class = "selected"` so both the
    // table highlight and the detail panel re-render with plain fields.
    void refresh_detail() {
        auto next_detail = std::make_shared<std::vector<ui::Row>>();
        auto next_peers = std::make_shared<std::vector<ui::PeerRow>>();
        if (!rows.empty()) {
            if (selected >= rows.size()) {
                selected = rows.size() - 1;
            }
            auto updated_rows =
                std::make_shared<std::vector<ui::Row>>(*rows.ptr);
            for (std::size_t i = 0; i < updated_rows->size(); ++i) {
                (*updated_rows)[i].selected_class =
                    (i == selected) ? "selected" : "";
            }
            rows = ui::RowList(std::move(updated_rows));
            auto sel = rows.at(selected);
            sel.detail_bar_width = detail_bar_width();
            *next_peers = sel.peer_list;
            next_detail->push_back(std::move(sel));
        }
        detail = ui::RowList(std::move(next_detail));
        selected_peers = ui::PeerList(std::move(next_peers));
        has_detail = !detail.empty();
        has_peers = !selected_peers.empty();
        no_peers = selected_peers.empty();
    }

    void scroll_selected_into_view() {
        auto wrap = QueryElement(".tablewrap");
        if (!wrap) return;
        const int row_y = static_cast<int>(selected) * 3;
        const int cur_y = wrap.scroll_y();
        if (row_y < cur_y) {
            wrap.SetScrollY(row_y);
        } else if (row_y > cur_y + 12) {
            wrap.SetScrollY(row_y - 12);
        }
    }

    void Move(int delta) {
        if (rows.empty()) return;
        const int n = static_cast<int>(rows.size());
        int cur = static_cast<int>(
            selected < rows.size() ? selected : rows.size() - 1);
        cur = ((cur + delta) % n + n) % n;
        selected = static_cast<std::size_t>(cur);
        refresh_detail();
        scroll_selected_into_view();
    }
    void Top() {
        if (!rows.empty()) {
            selected = 0;
            refresh_detail();
            scroll_selected_into_view();
        }
    }
    void Bottom() {
        if (!rows.empty()) {
            selected = rows.size() - 1;
            refresh_detail();
            scroll_selected_into_view();
        }
    }

    void ToggleDownload() {
        const ui::Row* r = current();
        if (!r) return;
        try {
            snap_.control(r->info_hash, 0);
            status_msg = "Toggled " + r->name;
        } catch (const std::exception& e) {
            status_msg = std::string("Error: ") + e.what();
        }
    }

    void Resume() {
        const ui::Row* r = current();
        if (!r) return;
        try {
            snap_.control(r->info_hash, 1);
            status_msg = "Resumed " + r->name;
        } catch (const std::exception& e) {
            status_msg = std::string("Error: ") + e.what();
        }
    }

    void Remove() {
        const ui::Row* r = current();
        if (!r) return;
        try {
            snap_.control(r->info_hash, 3);
            status_msg = "Removed " + r->name;
        } catch (const std::exception& e) {
            status_msg = std::string("Error: ") + e.what();
        }
    }

    // Remove the selected torrent and delete its downloaded data.
    void RemoveWithFiles() {
        const ui::Row* r = current();
        if (!r) return;
        try {
            snap_.control(r->info_hash, 2);
            status_msg = "Removed (with files) " + r->name;
        } catch (const std::exception& e) {
            status_msg = std::string("Error: ") + e.what();
        }
    }

    std::string_view view = R"html(
<div class="app">
  <div class="header">
    <span class="title">&#128420; Swarmbit</span>
    <span class="subtitle">dashboard</span>
    <span class="spacer"></span>
    <span class="help">{status_msg}</span>
  </div>

  <div class="tablewrap">
    <table class="list">
      <thead>
        <tr>
          <th class="col-name">Name</th>
          <th class="col-state">State</th>
          <th class="col-pct">Progress</th>
          <th class="col-down">Downloaded</th>
          <th class="col-down-rate">Down Rate</th>
          <th class="col-up">Uploaded</th>
          <th class="col-up-rate">Up Rate</th>
          <th class="col-peers">Peers</th>
        </tr>
      </thead>
      <tbody>
        <for each="{rows}" as="r">
        <tr class="row {r.selected_class}">
          <td class="col-name">{r.name}</td>
          <td class="col-state {r.state}">{r.state}</td>
          <td class="col-pct">
            <progress class="bar" value="{r.progress_pct_num}" max="100" width="16" />
            {r.progress_pct}
          </td>
          <td class="col-down">{r.downloaded_str}</td>
          <td class="col-down-rate">{r.down_rate_str}</td>
          <td class="col-up">{r.upload_total_str}</td>
          <td class="col-up-rate">{r.up_rate_str}</td>
          <td class="col-peers">{r.peers}</td>
        </tr>
        </for>
      </tbody>
    </table>
  </div>

  <div class="detail" if="{has_detail}">
    <for each="{detail}" as="d">
    <div class="detail-title">{d.name}</div>
    <div class="detail-hash">{d.info_hash}</div>
    <progress class="detailbar" value="{d.detail_progress_num}" max="100" width="{d.detail_bar_width}" />
    <div class="detail-stats">
      <span>Progress: {d.detail_progress}</span>
      <span>Size: {d.detail_size}</span>
      <span>Downloaded: {d.detail_downloaded}</span>
      <span>Uploaded: {d.detail_uploaded}</span>
      <span>Peers: {d.peers}</span>
      <span>Seeds: {d.seeds}</span>
      <span>Down: {d.detail_down_rate}</span>
      <span>Up: {d.detail_up_rate}</span>
      <span>ETA: {d.detail_eta}</span>
    </div>
    </for>
    <div class="detail-peers">
      <div class="detail-peers-header">Peers</div>
      <div class="detail-peers-empty" if="{no_peers}">No connected peers</div>
      <div class="detail-peers-list" if="{has_peers}">
        <table class="peer-table">
          <thead>
            <tr>
              <th class="col-peer-ep">Endpoint</th>
              <th class="col-peer-status">Status</th>
              <th class="col-peer-client">Client</th>
              <th class="col-peer-pct">Progress</th>
              <th class="col-peer-down">Down Rate</th>
              <th class="col-peer-up">Up Rate</th>
            </tr>
          </thead>
          <tbody>
            <for each="{selected_peers}" as="p">
            <tr class="peer-row">
              <td class="col-peer-ep">{p.endpoint}</td>
              <td class="col-peer-status">{p.status}</td>
              <td class="col-peer-client">{p.client}</td>
              <td class="col-peer-pct">{p.progress_pct}</td>
              <td class="col-peer-down">{p.down_rate_str}</td>
              <td class="col-peer-up">{p.up_rate_str}</td>
            </tr>
            </for>
          </tbody>
        </table>
      </div>
    </div>
    <div class="detail-keys">
      <span>[d] pause</span><span>[u] resume</span>
      <span>[x] remove</span><span>[Shift+x] remove+delete</span><span>[j/k] navigate</span><span>[q] quit</span>
    </div>
  </div>
</div>

<style>
  self {
    --bg: rgb(13, 17, 23);
    --panel: rgb(22, 27, 34);
    --border: rgb(48, 54, 61);
    --text: rgb(230, 237, 243);
    --muted: rgb(139, 148, 158);
    --accent: #00B8FF;
    --green: #C8B3F6;
    --amber: rgb(212, 175, 55);
    --red: rgb(248, 81, 73);
      --orange: #FF9900;
      --mint: #00FF9F;
      --magenta: #D600FF;
      --header: #00B8FF;

    display: flex;
    flex-direction: column;
    background-color: var(--bg);
    color: var(--text);
    width: 100%;
    height: 100%;
  }

  .app {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
  }

  .header {
    display: flex;
    align-items: center;
    padding: 1 3;
    border-bottom: solid;
    border-color: var(--border);
    background-color: var(--panel);
  }
  .title { color: var(--orange); font-weight: bold; }
  .subtitle { color: var(--muted); margin-left: 1; }
  .spacer { flex-grow: 1; }
  .help { color: var(--orange); }

  .tablewrap { flex-grow: 1; min-height: 8; overflow-y: auto; }
  .list { width: 100%; }
  th, td {
    padding: 0 2;
    text-align: left;
    white-space: nowrap;
  }
  .list td {
    padding: 1 2;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  thead th {
    position: sticky;
    top: 0;
    padding: 0 2;
    color: var(--header);
    border-bottom: solid;
    border-color: var(--border);
    background-color: var(--panel);
  }
  .row {
    white-space: nowrap;
  }
  .row.selected, .selected {
    background-color: rgba(214, 0, 255, 0.18);
    color: var(--magenta);
    font-weight: bold;
  }
  .selected td {
    color: var(--magenta);
  }
  .col-name {
    width: 48;
    flex-grow: 1;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    font-weight: bold;
  }
  table thead th {
    font-weight: bold;
    color: #00FFFF;
  }
  .col-state { width: 15; min-width: 15; flex-shrink: 0; white-space: nowrap; }
  .col-stateDownloading, .Downloading { color: var(--orange); }
  .col-stateSeeding, .Seeding { color: var(--green); }
  .col-stateCompleted, .Completed { color: var(--green); }
  .col-stateChecking, .Checking { color: var(--amber); }
  .col-pct { width: 27; min-width: 27; flex-shrink: 0; display: flex; align-items: center; gap: 1; white-space: nowrap; }
  .bar { color: var(--mint); background-color: var(--border); }
  .col-down { width: 14; min-width: 14; flex-shrink: 0; white-space: nowrap; }
  .col-down-rate { width: 14; min-width: 14; flex-shrink: 0; white-space: nowrap; }
  .col-up { width: 14; min-width: 14; flex-shrink: 0;  white-space: nowrap; }
  .col-up-rate { width: 14; min-width: 14; flex-shrink: 0; white-space: nowrap; }
  .col-peers { width: 9; min-width: 9; flex-shrink: 0; white-space: nowrap; }

  .detail {
    display: flex;
    flex-direction: column;
    flex-shrink: 0;
    border-top: solid;
    border-color: var(--border);
    background-color: var(--panel);
    padding: 1 3;
  }
  .detail-title {
    color: var(--accent);
    font-weight: bold;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .detail-hash { color: var(--muted); }
  .detailbar { color: var(--mint); background-color: var(--border); margin: 1 0; }
  .detail-stats { display: flex; flex-wrap: wrap; gap: 0 3; }
  .detail-stats span { color: var(--muted); }
  .detail-peers {
    display: flex;
    flex-direction: column;
    flex-shrink: 0;
    margin-top: 1;
  }
  .detail-peers-header { color: var(--header); font-weight: bold; }
  .detail-peers-empty { color: var(--muted); }
  .detail-peers-list {
    flex-shrink: 0;
    height: 22;
    min-height: 22;
    overflow-y: auto;
    scrollbar-width: none;
  }
  .peer-table { width: 100%; }
  .peer-row {
    height: 1;
    max-height: 1;
    white-space: nowrap;
  }
  .peer-table th {
    padding: 0 2 0 0;
    white-space: nowrap;
    color: var(--header);
    border-bottom: solid;
    border-color: var(--border);
    background-color: var(--panel);
  }
  .peer-table td {
    padding: 0 2 0 0;
    height: 1;
    max-height: 1;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .col-peer-ep { width: 34; color: var(--text); white-space: nowrap; }
  .col-peer-status { width: 33; color: var(--green); white-space: nowrap; }
  .col-peer-client { width: 22; color: var(--muted); white-space: nowrap; }
  .col-peer-pct { width: 10; color: var(--muted); white-space: nowrap; }
  .col-peer-down { width: 12; color: var(--muted); white-space: nowrap; }
  .col-peer-up { flex-grow: 1; color: var(--muted); white-space: nowrap; }
  .peer-table thead th { color: var(--header); }
  .detail-keys { color: var(--muted); margin-top: 1; }
  .detail-keys span { margin-right: 2; }
</style>
)html";

    explicit SwarmbitUI(Snapshot& snap) : snap_(snap) {
        Bind(rows, ui::RowFields);
        Bind(has_detail);
        Bind(has_peers);
        Bind(no_peers);
        Bind(selected);
        Bind(status_msg);
        Bind(current, &SwarmbitUI::current);
        BindCollection("detail", &SwarmbitUI::detail, ui::RowFields);
        BindCollection("selected_peers", &SwarmbitUI::selected_peers, ui::PeerFields);
        Import("Move", std::function<void(std::string)>([this](std::string s) { Move(std::stoi(s)); }));
        Bind(Top, &SwarmbitUI::Top);
        Bind(Bottom, &SwarmbitUI::Bottom);
        Bind(ToggleDownload, &SwarmbitUI::ToggleDownload);
        Bind(Resume, &SwarmbitUI::Resume);
        Bind(Remove, &SwarmbitUI::Remove);
    }

private:
    Snapshot& snap_;
};

namespace bt {
int run_ui(const std::string& preload) {
    Snapshot snapshot;

    // Start the monitor so libtorrent alerts are drained on a background
    // thread; the UI polls state directly via Snapshot. The monitor also
    // writes fast-resume data to the same directory restore_torrents() reads
    // (torrents were already restored in Snapshot's constructor).
    bt::AlertMonitor monitor(snapshot.session());
    monitor.set_resume_dir(snapshot.resume_dir_string());
    monitor.start();

    // Optionally add a torrent immediately on launch (used by `swarmbit add`).
    if (!preload.empty()) {
        snapshot.add(preload);
    }
    snapshot.refresh();

    auto app = Ref<SwarmbitUI>::New(snapshot);
    app->rows = snapshot.get_rows();
    app->refresh_detail();

    Screen screen(app);
    screen.SetInline(false);  // fullscreen: take over the whole terminal.
    app->screen_ = &screen;   // so 'q' can quit via OnEvent

    // Poll the live session on a background thread; push refreshed rows into
    // the component via RTXUI's task queue so the event loop wakes up and
    // re-renders without waiting for mouse/keyboard input. Keep the poller
    // alive until after Loop() returns so it never touches `snapshot` after
    // the local is destroyed.
    std::atomic<bool> polling{true};
    std::thread poller([&] {
        while (polling.load()) {
            snapshot.refresh();
            auto latest_rows = snapshot.get_rows();
            rtxui::PostTask([app, latest_rows = std::move(latest_rows)]() mutable {
                app->rows = std::move(latest_rows);
                app->refresh_detail();
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    screen.Loop();

    polling.store(false);
    poller.join();
    monitor.stop();
    // Flush up-to-date resume data for every torrent before the session is
    // torn down, so the next launch restores exact progress.
    monitor.save_all_resume_data();
    return 0;
}
}  // namespace bt
