# Swarmbit

A BitTorrent client written in C++ on top of
[libtorrent-rasterbar](https://libtorrent.org), with a fullscreen interactive
dashboard built on
[RTXUI](https://arthursonzogni.github.io/RTXUI/).

Swarmbit has two front-ends that share one backend:

- An **interactive RTXUI dashboard** (`add`, `ui`) with an add bar, a live
  torrent table, progress bars, and a detail panel.
- A set of **one-shot CLI commands** (`list`, `status`, `pause`, `resume`,
  `remove`, `ports`) for scripting and inspection.

## Features

- Add torrents from `.torrent` files or magnet links, with immediate display
  in the dashboard
- Live progress, peer counts, and up/down rate display
- Pause / resume / remove torrents by info-hash
- List active torrents and inspect their file layout
- Sparse file allocation (`storage_mode_sparse`) — disk space is allocated
  lazily as data is downloaded rather than pre-allocating the full file size
- Upload rate capped at 800 KB/s
- Clean shutdown on Ctrl-C (pauses all torrents, tears down the session)

## Requirements

- C++23 compiler (clang)
- CMake >= 3.16
- libtorrent-rasterbar and its dev headers
- RTXUI (header-only C++23 library)

### Installing dependencies

macOS (Homebrew):

```sh
brew install libtorrent-rasterbar rtxui
```

Debian/Ubuntu:

```sh
sudo apt-get install libtorrent-rasterbar-dev
```

RTXUI is header-only; build it from source or install via your package manager
if available.

## Build

```sh
cd swarmbit
cmake -S . -B build
cmake --build build
```

This produces `build/swarmbit` and `build/swarmbit-tests`.

## Usage

```sh
# Launch the RTXUI dashboard with a torrent preloaded (blocks until q/Esc)
./build/swarmbit add path/to/file.torrent
./build/swarmbit add "magnet:?xt=urn:btih:..."

# Launch the RTXUI dashboard with an empty list
./build/swarmbit ui

# One-shot CLI commands
./build/swarmbit list
./build/swarmbit list --verbose
./build/swarmbit status <info-hash>
./build/swarmbit pause <info-hash>
./build/swarmbit resume <info-hash>
./build/swarmbit remove <info-hash> [--files]
./build/swarmbit ports
```

Info-hashes may be given as 40-char hex, optionally prefixed with `btih:`.

## Dashboard controls

| Key            | Action                      |
|----------------|-----------------------------|
| `j` / Down     | select next                 |
| `k` / Up       | select previous             |
| `g`            | go to top                   |
| `G`            | go to bottom                |
| `Enter`        | add (from the add bar)      |
| `d`            | toggle download / pause     |
| `u`            | resume                      |
| `x`            | remove (with files)         |
| `q` / Esc      | quit                        |

## Project layout

| Path | Purpose |
|------|---------|
| `src/session_manager.{hpp,cpp}` | RAII wrapper over `libtorrent::session` |
| `src/torrent_handle.{hpp,cpp}`  | Per-torrent state queries and lifecycle |
| `src/alert_monitor.{hpp,cpp}`  | Background alert-draining thread |
| `src/ui.{hpp,cpp}`             | RTXUI dashboard (table, detail panel, add bar) |
| `src/main.cpp`                 | CLI entry point and command dispatch |
| `tests/test_torrent.cpp`       | Self-contained unit checks |

## Testing

```sh
ctest --test-dir build --output-on-failure
```

## Notes on the RTXUI binding

RTXUI templates do not evaluate expressions (`{r.progress * 100}` renders
literally), so every displayed value is a pre-computed field on `ui::Row`
(`progress_pct`, `downloaded_str`, `up_rate_str`, and the detail-panel fields).
The torrent list is bound through a `RowList` range adapter wrapping a
`shared_ptr<vector<Row>>`; the poller replaces the struct with a fresh copy of
the underlying vector each tick so RTXUI's change detection re-renders live
values.
