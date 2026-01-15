#include "torrent_manager.hpp"
#include "http_server.hpp"
#include "websocket_server.hpp"
#include <logging.hpp>
#include <filesystem>
#include <iostream>
#include <csignal>
#include <cstdlib>

namespace fs = std::filesystem;

volatile std::atomic<bool> running{true};

void signal_handler(int signal) {
    running = false;
}

int main(int argc, char* argv[]) {
    // Set up signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize logging
    pixellib::core::logging::Logger::set_level(pixellib::core::logging::LOG_DEBUG);
    const char* home_env = std::getenv("HOME");
    if (!home_env) {
        throw std::runtime_error("HOME environment variable not set");
    }
    fs::path config_dir = fs::path(home_env) / ".config" / "pixelscrape";
    fs::create_directories(config_dir);
    std::string log_file = (config_dir / "pixelscrape.log").string();
    pixellib::core::logging::Logger::set_file_logging(log_file, 10 * 1024 * 1024, 5); // 10MB, 5 files

    std::cout << "Starting PixelScrape Torrent Client..." << std::endl;
    pixellib::core::logging::Logger::info("Starting PixelScrape Torrent Client");

    try {
        // Create directories
        fs::path download_dir = "downloads";
        fs::path state_dir = config_dir / "state";
        fs::create_directories(download_dir);
        fs::create_directories(state_dir);

        // Initialize torrent manager
        pixelscrape::TorrentManager torrent_manager(download_dir, state_dir);

        // Initialize HTTP server
        pixelscrape::HttpServer http_server(8080);

        // API endpoints
        http_server.add_route("GET", "/api/torrents", [&torrent_manager](const pixelscrape::HttpRequest& req) {
            pixelscrape::HttpResponse response;
            response.headers["Content-Type"] = "application/json";
            response.headers["Access-Control-Allow-Origin"] = "*";

            auto torrents = torrent_manager.list_torrents();
            pixellib::core::json::JSON torrent_array = pixellib::core::json::JSON::array({});

            for (const auto& torrent_id : torrents) {
                auto status = torrent_manager.get_torrent_status(torrent_id);
                if (status) {
                    torrent_array.push_back(*status);
                }
            }

            response.body = torrent_array.stringify(pixellib::core::json::StringifyOptions{.pretty = true});
            return response;
        });

        http_server.add_route("POST", "/api/torrents", [&torrent_manager](const pixelscrape::HttpRequest& req) {
            pixelscrape::HttpResponse response;
            response.headers["Content-Type"] = "application/json";
            response.headers["Access-Control-Allow-Origin"] = "*";

            try {
                // Parse JSON body for torrent path and file priorities
                auto json_value = pixellib::core::json::JSON::parse_or_throw(req.body);
                if (!json_value.is_object()) {
                    throw std::runtime_error("Invalid JSON");
                }

                auto path_it = json_value.find("path");
                if (!path_it || !path_it->is_string()) {
                    throw std::runtime_error("Missing torrent path");
                }

                std::string torrent_path = path_it->as_string();
                std::vector<size_t> file_priorities;

                auto priorities_it = json_value.find("file_priorities");
                if (priorities_it && priorities_it->is_array()) {
                    const auto& priorities_array = priorities_it->as_array();
                    for (const auto& priority : priorities_array) {
                        if (priority.is_number()) {
                            file_priorities.push_back(static_cast<size_t>(priority.as_number().to_int64()));
                        }
                    }
                }

                std::string torrent_id = torrent_manager.add_torrent(torrent_path, file_priorities);

                pixellib::core::json::JSON result = pixellib::core::json::JSON::object({});
                result["success"] = pixellib::core::json::JSON(true);
                result["torrent_id"] = pixellib::core::json::JSON(torrent_id);
                response.body = result.stringify(pixellib::core::json::StringifyOptions{.pretty = true});

            } catch (const std::exception& e) {
                response.status_code = 400;
                pixellib::core::json::JSON error = pixellib::core::json::JSON::object({});
                error["success"] = pixellib::core::json::JSON(false);
                error["error"] = pixellib::core::json::JSON(e.what());
                response.body = error.stringify(pixellib::core::json::StringifyOptions{.pretty = true});
            }

            return response;
        });

        http_server.add_route("DELETE", "/api/torrents/{id}", [&torrent_manager](const pixelscrape::HttpRequest& req) {
            pixelscrape::HttpResponse response;
            response.headers["Content-Type"] = "application/json";
            response.headers["Access-Control-Allow-Origin"] = "*";

            // Extract torrent ID from path (simplified)
            std::string path = req.path;
            size_t id_pos = path.find("/api/torrents/");
            if (id_pos != std::string::npos) {
                std::string torrent_id = path.substr(id_pos + 14); // Length of "/api/torrents/"

                bool success = torrent_manager.remove_torrent(torrent_id);
                pixellib::core::json::JSON result = pixellib::core::json::JSON::object({});
                result["success"] = pixellib::core::json::JSON(success);
                response.body = result.stringify(pixellib::core::json::StringifyOptions{.pretty = true});

                if (!success) {
                    response.status_code = 404;
                }
            } else {
                response.status_code = 400;
            }

            return response;
        });

        http_server.add_route("GET", "/api/stats", [&torrent_manager](const pixelscrape::HttpRequest& req) {
            pixelscrape::HttpResponse response;
            response.headers["Content-Type"] = "application/json";
            response.headers["Access-Control-Allow-Origin"] = "*";

            auto stats = torrent_manager.get_global_stats();
            response.body = stats.stringify(pixellib::core::json::StringifyOptions{.pretty = true});
            return response;
        });

        // Initialize WebSocket server
        pixelscrape::WebSocketServer ws_server(8081);

        // Start servers
        http_server.start();
        ws_server.start();

        std::cout << "\nPixelScrape started successfully!" << std::endl;
        std::cout << "HTTP API available at: http://localhost:8080" << std::endl;
        std::cout << "WebSocket available at: ws://localhost:8081" << std::endl;
        std::cout << "Press Ctrl+C to stop...\n" << std::endl;
        
        pixellib::core::logging::Logger::info("PixelScrape started - HTTP server on port 8080, WebSocket on port 8081");

        // Main loop
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Broadcast stats every 5 seconds
            static int counter = 0;
            if (++counter % 5 == 0) {
                auto stats = torrent_manager.get_global_stats();
                std::string stats_json = stats.stringify(pixellib::core::json::StringifyOptions{.pretty = false});
                ws_server.broadcast_text(stats_json);
            }
        }

        std::cout << "\nShutting down PixelScrape..." << std::endl;
        pixellib::core::logging::Logger::info("Shutting down PixelScrape");

        // Stop servers
        ws_server.stop();
        http_server.stop();

    } catch (const std::exception& e) {
        std::cerr << "\nFatal error: " << e.what() << std::endl;
        pixellib::core::logging::Logger::error("Fatal error: {}", e.what());
        return 1;
    }

    return 0;
}