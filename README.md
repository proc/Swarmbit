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
- Live progress, peer counts, up/down rate, and ETA display
- Pause / resume / remove torrents by info-hash, optionally deleting the
  downloaded data
- List active torrents and inspect their file layout
- Sparse file allocation (`storage_mode_sparse`) — disk space is allocated
  lazily as data is downloaded rather than pre-allocating the full file size
- Configurable download directory — defaults to `~/Downloads` ($HOME/Downloads),
  customizable via CLI option (`--download-dir`), config file (`~/.swarmbit/config`),
  or environment variable (`SWARMBIT_DOWNLOAD_DIR`)
- Upload rate capped at 800 KB/s
- Resume persistence — the dashboard saves libtorrent fast-resume data to
  `~/.swarmbit/resume/` and restores it on
  the next launch, so in-progress and completed torrents come back with their
  progress (no full recheck) and their paused/running state. See
  [Persistence](#persistence)
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

# Configuration
./build/swarmbit config download-dir
./build/swarmbit config download-dir ~/MyDownloads
```

Downloads default to `~/Downloads`. You can customize the download directory in three ways:

1. **CLI Flag**: `./build/swarmbit --download-dir /path/to/downloads [add|ui]` (or `-o <path>`)
2. **Persistent Config**: `./build/swarmbit config download-dir /path/to/downloads` (stored in `~/.swarmbit/config`)
3. **Environment Variable**: `export SWARMBIT_DOWNLOAD_DIR="/path/to/downloads"`

Info-hashes may be given as 40-char hex, optionally prefixed with `btih:`.

`remove` drops the torrent from the session but leaves its files on disk;
add `--files` to also delete the downloaded data.

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
| `x`            | remove torrent (keep downloaded files) |
| `Shift+x`      | remove torrent and delete its downloaded files |
| `q` / Esc      | quit                        |

`Shift+x` is matched on the uppercase `X` character itself, since most
terminals don't report the Shift modifier for printable keys. Caps Lock + `x`
therefore also deletes data.

### Detail panel

Selecting a torrent opens a detail panel with its progress bar, size,
downloaded/uploaded totals, peer and seed counts, up/down rates, an
**ETA**, and the list of connected peers.

The ETA is the remaining wanted bytes (skipped files excluded) divided by the
current payload download rate, rounded up:

| Display               | Meaning |
|-----------------------|---------|
| `2d 03h`, `1h 05m`, `4m 09s`, `37s` | estimated time remaining (two most significant units) |
| `Done`                | finished / seeding |
| `∞`                   | unknown: paused, checking files, fetching metadata, no data flowing, or more than 100 days away |

The ETA uses the payload rate (file data only), so it can differ slightly
from the `Down` rate shown, which includes protocol overhead. It is not
smoothed, so it moves with the instantaneous rate.

## Persistence

The dashboard keeps its state in `~/.swarmbit/` in the user's home directory:

| Path | Contents |
|------|----------|
| `~/.swarmbit/config` | user configuration (e.g. `download_dir = ~/Downloads`) |
| `~/.swarmbit/resume/<v1>_<v2>.resume` | libtorrent fast-resume data per torrent (progress, paused state, save path, and the embedded metadata) |
| `~/.swarmbit/downloading.tsv` | torrents that are actively downloading (info-hash, save path, source file or magnet) |
| `~/.swarmbit/torrents/<hash>.torrent` | cached copy of each torrent's metadata |

Resume data is written:

- when a torrent is added, a magnet link resolves its metadata, a download
  finishes, or a torrent is paused or resumed
- every 30 seconds, for torrents whose progress or state changed
- for every torrent when you quit (`q` / Esc), before the session shuts down

Resume files are written to a temporary file and renamed, so a crash
mid-write can't leave a truncated file. Removing a torrent (`x` or
`Shift+x`) deletes its resume file so it is not restored again.

On launch, torrents are restored from `~/.swarmbit/resume/` first; anything in
`downloading.tsv` without a resume file (e.g. added just before a crash) is
then re-added from the cached `.torrent` file or magnet link and rechecked.

> The one-shot CLI commands (`list`, `status`, `pause`, `resume`, `remove`)
> open a fresh session and do not load this state.

## Project layout

| Path | Purpose |
|------|---------|
| `src/session_manager.{hpp,cpp}` | RAII wrapper over `libtorrent::session` |
| `src/torrent_handle.{hpp,cpp}`  | Per-torrent state queries and lifecycle |
| `src/alert_monitor.{hpp,cpp}`  | Background alert-draining thread; writes/deletes fast-resume data |
| `src/ui.{hpp,cpp}`             | RTXUI dashboard (table, detail panel, add bar) |
| `src/main.cpp`                 | CLI entry point and command dispatch |
| `tests/test_torrent.cpp`       | Self-contained unit checks |

## Testing

```sh
ctest --test-dir build --output-on-failure
```

The suite runs offline. Besides magnet/hex parsing and session checks, it
seeds a tiny locally generated torrent to verify that `remove(true)` deletes
the payload from disk and `remove(false)` keeps it, that resume data
round-trips (written while running and on shutdown, restored with its paused
state, deleted on removal), and that the ETA reports unknown before metadata
is available.

## Notes on the RTXUI binding

RTXUI templates do not evaluate expressions (`{r.progress * 100}` renders
literally), so every displayed value is a pre-computed field on `ui::Row`
(`progress_pct`, `downloaded_str`, `up_rate_str`, and the detail-panel fields).
The torrent list is bound through a `RowList` range adapter wrapping a
`shared_ptr<vector<Row>>`; the poller replaces the struct with a fresh copy of
the underlying vector each tick so RTXUI's change detection re-renders live
values.
