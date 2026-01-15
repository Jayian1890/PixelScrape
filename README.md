# PixelScrape

A BitTorrent client written in C++ with a web-based user interface.

## Features

- Torrent downloading and seeding
- Web UI for management
- REST API for torrent operations
- Real-time statistics via WebSocket

## Build Instructions

### Prerequisites

- CMake 3.16 or higher
- C++17 compatible compiler

### Building

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Running

```bash
./pixelscrape
```

The application starts:
- HTTP API server on port 8080
- WebSocket server on port 8081 for real-time stats

## API Endpoints

- `GET /api/torrents` - List all torrents
- `POST /api/torrents` - Add a new torrent (JSON body: `{"path": "/path/to/torrent", "file_priorities": [1,0,...]}`)
- `DELETE /api/torrents/{id}` - Remove a torrent
- `GET /api/stats` - Get global statistics

## Architecture

- **TorrentManager**: Orchestrates torrent lifecycle
- **Bencode Parser**: Handles .torrent file parsing
- **Tracker Client**: Communicates with torrent trackers
- **Peer Connections**: Manages peer-to-peer connections
- **Piece Manager**: Handles piece downloading and verification
- **HTTP/WebSocket Servers**: Provide web interface and API

## Dependencies

Self-contained with vendored header-only libraries (`pixellib`).

## License

MIT License - see [LICENSE](LICENSE) file for details.