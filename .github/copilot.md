# Copilot / AI agent instructions for PixelScrape

Goal: make an AI coding agent productive quickly by documenting the project shape, common workflows, and repository conventions.

## Quick start (build & run)
- **Build System**: CMake.
- **Configure**: `cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug`.
- **Build**: `cmake --build build`.
- **Run**: `./pixelscrape` (starts HTTP API on 8080, WebSocket on 8081).

## Big picture (short)
- **Architecture**: BitTorrent client with core components for torrent lifecycle management, peer connections, piece storage, and web API.
- **Components**: `TorrentManager` orchestrates `Torrent` structs; each torrent has metadata, tracker client, piece manager, peer connections.
- **Data Flow**: Load .torrent file → parse bencode → contact tracker for peers → establish peer connections → download/verify pieces → save to files.
- **Web UI**: HTTP server for REST API (add/remove torrents), WebSocket for real-time stats broadcast.

## Project conventions & patterns
- **Language**: C++17.
- **Namespaces**: `pixelscrape` (project code), `pixellib::core` (header-only utilities).
- **Classes**: `PascalCase` (e.g., `TorrentManager`, `HttpServer`).
- **Methods/Functions**: `snake_case` (e.g., `add_torrent`, `get_global_stats`).
- **Variables**: `snake_case`, member variables end with `_` (e.g., `download_dir_`, `mutex_`).
- **Async**: `std::thread` for background workers (e.g., `tracker_thread`, `peer_worker`).
- **Logging**: `pixellib::core::logging::Logger::info/error` with file rotation.
- **JSON**: `pixellib::core::json::JSON` for API responses.
- **Filesystem**: `std::filesystem` for paths and directories.
- **Dependencies**: Self-contained; uses vendored `pixellib` (header-only: logging, json, filesystem, network).

## Git Workflow
- **Conventional Commits**: `feat:`, `fix:`, `docs:`, etc.
- **Atomic Task Commits**: Commit each distinct task immediately.

## Files to read for understanding
- **Entry**: `src/main.cpp` (server wiring, API routes, main loop).
- **Core Logic**: `include/torrent_manager.hpp` and `src/torrent_manager.cpp` (torrent lifecycle, status).
- **Parsing**: `src/bencode/parser.cpp` (bencode decoding).
- **Storage**: `src/storage/piece_manager.cpp` (piece verification/storage).
- **Network**: `src/tracker/client.cpp` and `src/peer/connection.cpp` (tracker/peer protocols).