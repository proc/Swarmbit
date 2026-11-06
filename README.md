# bittorrent-client

A command-line BitTorrent client written in C++17 on top of
[libtorrent-rasterbar](https://libtorrent.org).

## Features

- Add torrents from `.torrent` files or magnet links
- Live progress, peer counts, and up/down rate display
- Pause / resume / remove torrents by info-hash
- List active torrents and inspect their file layout
- Clean shutdown on Ctrl-C (pauses all torrents, tears down the session)

## Requirements

- C++17 compiler (clang or gcc)
- CMake >= 3.16
- libtorrent-rasterbar and its dev headers

### Installing libtorrent

macOS (Homebrew):

```sh
brew install libtorrent-rasterbar
```

Debian/Ubuntu:

```sh
sudo apt-get install libtorrent-rasterbar-dev
```

## Build

```sh
cd bittorrent-client
cmake -S . -B build
cmake --build build
```

This produces `build/bt-client` and `build/bittorrent-tests`.

## Usage

```sh
# Add a torrent file or magnet link (saves into the current directory)
./build/bt-client add path/to/file.torrent
./build/bt-client add "magnet:?xt=urn:btih:..."

# List active torrents
./build/bt-client list
./build/bt-client list --verbose

# Show detailed status of one torrent
./build/bt-client status <info-hash>

# Pause / resume / remove (use --files to delete downloaded data too)
./build/bt-client pause <info-hash>
./build/bt-client resume <info-hash>
./build/bt-client remove <info-hash> [--files]

# Show listening ports
./build/bt-client ports
```

Info-hashes may be given as 40-char hex, optionally prefixed with `btih:`.

## Project layout

| Path | Purpose |
|------|---------|
| `src/session_manager.{hpp,cpp}` | RAII wrapper over `libtorrent::session` |
| `src/torrent_handle.{hpp,cpp}`  | Per-torrent state queries and lifecycle |
| `src/alert_monitor.{hpp,cpp}`  | Background alert-draining thread |
| `src/main.cpp`                 | CLI entry point and event loop |
| `tests/test_torrent.cpp`       | Self-contained unit checks |

## Testing

```sh
ctest --test-dir build --output-on-failure
```
