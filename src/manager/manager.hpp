#pragma once

// Manager module internal header
// Consolidates common includes for manager sub-modules

#include "filesystem.hpp"
#include "torrent_manager.hpp"
#include <chrono>
#include <json.hpp>
#include <random>

// Sockets for incoming peer listener
#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
