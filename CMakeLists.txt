cmake_minimum_required(VERSION 3.16)
project(PixelScrape VERSION 1.0.0 LANGUAGES CXX)

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add pixelLib submodule
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/pixellib")
    message(FATAL_ERROR "pixelLib submodule not found. Run: git submodule update --init --recursive")
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/doctest
    ${CMAKE_CURRENT_SOURCE_DIR}/external/pixellib/include
)

# Source files
set(SOURCES
    src/main.cpp
    src/torrent_manager.cpp
    src/bencode/parser.cpp
    src/crypto/sha1.cpp
    src/torrent/metadata.cpp
    src/tracker/client.cpp
    src/peer/connection.cpp
    src/storage/piece_manager.cpp
    src/storage/state_manager.cpp
    src/api/http_server.cpp
    src/api/websocket_server.cpp
    src/api/transmission_rpc.cpp
)

# Create executable
add_executable(pixelscrape ${SOURCES})

# Compiler flags
target_compile_options(pixelscrape PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O2
)

# Define DOCTEST_CONFIG_DISABLE to disable doctest in release builds
target_compile_definitions(pixelscrape PRIVATE DOCTEST_CONFIG_DISABLE)

# Link libraries (none needed for pixelLib as it's header-only)
# target_link_libraries(pixelscrape ...)
# target_link_libraries(pixelscrape c++) # This line is incorrect, removing it
target_link_libraries(pixelscrape pthread)

# Install targets
install(TARGETS pixelscrape DESTINATION bin)

# Copy frontend files to build directory
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/frontend/index.html ${CMAKE_CURRENT_BINARY_DIR}/frontend/index.html COPYONLY)