# Copilot / AI agent instructions for BitScrape

Goal: make an AI coding agent productive quickly by documenting the project shape, common workflows, and repository conventions.

## Quick start (build & run)
- **Build System**: CMake (replace any references to `make` with `cmake`).
- **Configure**: `cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug` (or `Release`).
- **Build**: `cmake --build build` (parallel build: `cmake --build build -j$(nproc)`).
- **Run CLI**: `./build/bin/bitscrape_cli --help`.
- **Note on Tests**: The `tests` directory may not be present in the current workspace view; if available, standard CMake testing would be `ctest --test-dir build`.

## Big picture (short)
- **Architecture**: Modular, event-driven. Modules are static libraries, apps link them.
- **Directory Structure**:
  - `include/bitscrape/<module>/`: Public headers.
  - `modules/<module>/src/`: Module implementation files.
  - `apps/`: Applications and UIs (e.g. `apps/cli`, `apps/web`).
  - `CMakeLists.txt`: Root and per-module build definitions.
- **Key Modules**:
  - `modules/core`: Controller (orchestration), Configuration.
  - `modules/dht`, `modules/bittorrent`: Protocol implementations.
  - `modules/storage`: SQLite database wrapper (`StorageManager`).
  - `modules/event`: Event bus system.
  - `modules/beacon`: Logging system.
- **Applications**:
  - `apps/cli`: Command-line interface.
  - `apps/web`: Web UI (HTTP server, API handlers, WebSocket).

## Project conventions & patterns an agent should follow
- **Language**: C++20 is required.
- **Build System**: **CMake** is the source of truth. Ensure `CMakeLists.txt` files are updated when adding files.
- **Headers**: Always place public headers in `include/bitscrape/<module>/`.
- **Implementation**: Use the Pimpl idiom (`class Impl`) in `.cpp` files to hide private details (e.g., `Controller::Impl` in `modules/core/src/controller.cpp`).
- **Naming**:
  - **Namespaces**: Nested `bitscrape::<module>` (e.g., `bitscrape::core`).
  - **Classes**: `PascalCase` (e.g., `StorageManager`).
  - **Methods/Functions**: `snake_case` (e.g., `start_async`, `get_string`).
  - **Variables**: `snake_case` (members often end with `_`, e.g., `config_`, `is_running_`).
- **Logging**: Use `bitscrape::beacon::Beacon`. Typically `beacon_->info("Message", types::BeaconCategory::GENERAL)`.
- **Async**: Heavy use of `std::future`, `std::async` for non-blocking operations.
- **Dependencies**: Codebase is self-contained (vendored `doctest` if used), minimal external deps.
- **Git Commit Messages**: Follow the Conventional Commits specification:
  - Format: `<type>(<scope>): <subject>`
  - Types:
    - `feat`: New feature
    - `fix`: Bug fix
    - `docs`: Documentation only changes
    - `style`: Changes that do not affect the meaning of the code (white-space, formatting, etc)
    - `refactor`: A code change that neither fixes a bug nor adds a feature
    - `perf`: A code change that improves performance
    - `test`: Adding missing tests or correcting existing tests
    - `build`: Changes that affect the build system or external dependencies
    - `ci`: Changes to our CI configuration files and scripts
    - `chore`: Other changes that don't modify src or test files
    - `revert`: Reverts a previous commit
- **Git Workflow**:
  - **Atomic Task Commits**: AI MUST commit and push changes immediately after completing each distinct user task or logical unit of work.
  - **No Batching**: Do NOT combine changes from separate tasks into a single commit. Process each request's changes independently to maintain a clean and revertible history.

## Files an agent should read to understand code paths quickly
- **Root Build**: `CMakeLists.txt` (Project settings, compiler flags).
- **App Entry**: `apps/cli/src/main.cpp` (CLI setup, wiring).
- **Orchestration**: `modules/core/src/controller.cpp` (The brain of the app, wiring modules).
- **Configuration**: `include/bitscrape/core/configuration.hpp`.
- **Storage**: `modules/storage/src/storage_manager.cpp`.

## Debugging & Workflows
- **Debug Build**: Ensure `-DCMAKE_BUILD_TYPE=Debug` is used.
- **Run**: Execute `./build/bin/bitscrape_cli`.
- **VS Code**: Use CMake Tools extension. `Ctrl+Shift+P` -> `CMake: Configure` -> `CMake: Build`.

## Integration points
- **Database**: SQLite DB file defaults to `data/bitscrape.db` (created by `modules/storage`).
- **Network**: Binds to ports (default 6881) for DHT/BitTorrent.