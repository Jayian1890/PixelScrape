cmake_minimum_required(VERSION 3.10)
project(PixelScrape)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Find OpenSSL
find_package(OpenSSL REQUIRED)
if(OPENSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
else()
    message(FATAL_ERROR "OpenSSL not found")
endif()

# Include directories
include_directories(include)
include_directories(${OPENSSL_INCLUDE_DIR})

# Find all source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Create executable
add_executable(PixelScrape ${SOURCES})

# Link libraries
target_link_libraries(PixelScrape OpenSSL::SSL OpenSSL::Crypto)
