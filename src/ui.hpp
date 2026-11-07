// RTXUI dashboard entry point for Swarmbit.
//
// run_ui() launches the fullscreen interactive UI (add bar, torrent table,
// detail panel) backed by a persistent libtorrent session. Pass `preload`
// (torrent file path or magnet URI) to add a torrent immediately on launch;
// pass an empty string to start with an empty list.
#pragma once

#include <string>
#include <memory>
#include <vector>

#include <rtxui/rtxui.hpp>

namespace bt {

// Run the RTXUI interface. Blocks until the user quits (q / Esc).
int run_ui(const std::string& preload = "");

}  // namespace bt

// Row and Fields live in a named namespace so RTXUI's <for> loop can find
// Fields() via argument-dependent lookup (or via the explicit Bind mapper).
//
// RTXUI templates do NOT evaluate expressions or function calls (e.g.
// `{r.progress * 100}` or `{human_bytes(r.downloaded)}` are shown literally).
// Every value the template reads must therefore be a plain field, so the
// pre-computed *_str fields below hold the final rendered text.
namespace ui {

// One peer entry in the selected torrent's info box.
struct PeerRow {
    std::string endpoint;
    std::string client;
    std::string status;
    std::string progress_pct;
    std::string down_rate_str;
    std::string up_rate_str;

    bool operator==(const PeerRow& other) const = default;
};

void PeerFields(const PeerRow& p, rtxui::FieldSink& out);
inline void Fields(const PeerRow& p, rtxui::FieldSink& out) {
    PeerFields(p, out);
}

// One row in the torrent list.
struct Row {
    std::string info_hash;
    std::string name;
    std::string state;
    double progress = 0.0;
    long long size = 0;
    long long downloaded = 0;
    long long upload_total = 0;
    int peers = 0;
    int seeds = 0;
    long long down_rate = 0;
    long long up_rate = 0;
    bool finished = false;

    // Pre-computed display strings so the template can use simple field
    // access only (RTXUI does not evaluate expressions in templates).
    std::string selected_class;     // "selected" when row is selected, else ""
    double progress_pct_num = 0.0;  // 0..100 for the <progress> value attr
    std::string progress_pct;   // e.g. "42.0%"
    std::string downloaded_str; // e.g. "1.0 MB"
    std::string upload_total_str; // e.g. "0 B"
    std::string down_rate_str;  // e.g. "100.0 KB/s"
    std::string up_rate_str;    // e.g. "50.0 KB/s"
    // Pre-computed detail-panel values (RTXUI cannot evaluate expressions).
    std::string detail_progress;  // e.g. "42.0%"
    long long detail_progress_num = 0.0;  // 0..100 for <progress> value
    std::string detail_size;      // e.g. "1.0 GB"
    std::string detail_downloaded;
    std::string detail_uploaded;
    std::string detail_down_rate;
    std::string detail_up_rate;
    std::vector<PeerRow> peer_list;

    bool operator==(const Row& other) const = default;
};

// Fields() lets RTXUI's <for> loop read Row members for interpolation.
void RowFields(const Row& r, rtxui::FieldSink& out);
void Fields(const Row& r, rtxui::FieldSink& out);

// The bound collection. RTXUI's <for> creates its per-item visitor once, at
// the first render (when the list is empty), then only snapshots the container
// in place. A std::vector would be swapped out from under that snapshot,
// leaving the visitor reading stale/empty values — the fields then render as
// raw template expressions. Binding a stable shared_ptr lets the same
// TypeErasedRangeImpl re-read live contents on every render instead.
//
// RTXUI requires the bound type itself to be a std::ranges::range (it does
// not unwrap shared_ptr), so RowList is a range adapter: it forwards the
// range interface to the vector held through a shared_ptr. The pointer stays
// stable across polls (we clear()+assign its contents in place), so RTXUI
// re-reads live values each frame.
struct RowList {
    std::shared_ptr<std::vector<ui::Row>> ptr =
        std::make_shared<std::vector<ui::Row>>();
    RowList() = default;
    explicit RowList(std::shared_ptr<std::vector<ui::Row>> p) : ptr(std::move(p)) {}
    using range_value_t = ui::Row;

    auto begin() { return ptr->begin(); }
    auto end() { return ptr->end(); }
    auto begin() const { return ptr->begin(); }
    auto end() const { return ptr->end(); }
    auto size() const { return ptr->size(); }
    bool empty() const { return ptr->empty(); }
    ui::Row& at(std::size_t i) { return ptr->at(i); }
    const ui::Row& at(std::size_t i) const { return ptr->at(i); }
    ui::Row& back() { return ptr->back(); }
    const ui::Row& back() const { return ptr->back(); }
    ui::Row& operator[](std::size_t i) { return (*ptr)[i]; }
    const ui::Row& operator[](std::size_t i) const { return (*ptr)[i]; }

    // Required by RTXUI's TypeErasedRangeImpl, which copies the container and
    // compares it to detect changes. Deep-copy the underlying vector so
    // RTXUI's snapshot never aliases live state.
    RowList(const RowList& other)
        : ptr(std::make_shared<std::vector<ui::Row>>(*other.ptr)) {}
    RowList& operator=(const RowList& other) {
        if (this != &other) {
            ptr = std::make_shared<std::vector<ui::Row>>(*other.ptr);
        }
        return *this;
    }
    RowList& operator=(RowList&& other) noexcept {
        ptr = std::move(other.ptr);
        return *this;
    }
    friend bool operator!=(const RowList& a, const RowList& b) {
        return *a.ptr != *b.ptr;
    }
    friend bool operator==(const RowList& a, const RowList& b) {
        return *a.ptr == *b.ptr;
    }
};

// Range adapter for the selected torrent's peer list, with deep-copy semantics
// matching RowList so RTXUI's snapshot comparisons work reliably.
struct PeerList {
    std::shared_ptr<std::vector<ui::PeerRow>> ptr =
        std::make_shared<std::vector<ui::PeerRow>>();
    PeerList() = default;
    explicit PeerList(std::shared_ptr<std::vector<ui::PeerRow>> p)
        : ptr(std::move(p)) {}
    using range_value_t = ui::PeerRow;

    auto begin() { return ptr->begin(); }
    auto end() { return ptr->end(); }
    auto begin() const { return ptr->begin(); }
    auto end() const { return ptr->end(); }
    auto size() const { return ptr->size(); }
    bool empty() const { return ptr->empty(); }
    ui::PeerRow& at(std::size_t i) { return ptr->at(i); }
    const ui::PeerRow& at(std::size_t i) const { return ptr->at(i); }
    ui::PeerRow& back() { return ptr->back(); }
    const ui::PeerRow& back() const { return ptr->back(); }
    ui::PeerRow& operator[](std::size_t i) { return (*ptr)[i]; }
    const ui::PeerRow& operator[](std::size_t i) const { return (*ptr)[i]; }

    PeerList(const PeerList& other)
        : ptr(std::make_shared<std::vector<ui::PeerRow>>(*other.ptr)) {}
    PeerList& operator=(const PeerList& other) {
        if (this != &other) {
            ptr = std::make_shared<std::vector<ui::PeerRow>>(*other.ptr);
        }
        return *this;
    }
    PeerList& operator=(PeerList&& other) noexcept {
        ptr = std::move(other.ptr);
        return *this;
    }
    friend bool operator!=(const PeerList& a, const PeerList& b) {
        return *a.ptr != *b.ptr;
    }
    friend bool operator==(const PeerList& a, const PeerList& b) {
        return *a.ptr == *b.ptr;
    }
};

}  // namespace ui
