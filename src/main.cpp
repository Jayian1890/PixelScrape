#include "torrent_manager.hpp"
#include "http_server.hpp"
#include "websocket_server.hpp"
#include "transmission_rpc.hpp"
#include <logging.hpp>
#include <filesystem>
#include <iostream>
#include <csignal>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;

volatile std::atomic<bool> running{true};

void signal_handler(int signal) {
    (void)signal;
    running = false;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
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

        // Initialize Transmission RPC handler
        pixelscrape::TransmissionRpcHandler transmission_handler(torrent_manager);

        // Initialize HTTP server
        pixelscrape::HttpServer http_server(8080);

        // API endpoints

        // Serve the web UI
        http_server.add_route("GET", "/", [](const pixelscrape::HttpRequest&) {
            pixelscrape::HttpResponse response;
            response.headers["Content-Type"] = "text/html";

            std::ifstream file("frontend/index.html");
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                response.body = buffer.str();
                file.close();
            } else {
                response.status_code = 500;
                response.status_message = "Internal Server Error";
                response.body = "Failed to load index.html";
            }

            return response;
        });
        http_server.add_route("POST", "/transmission/rpc", [&transmission_handler](const pixelscrape::HttpRequest& req) {
            return transmission_handler.handle_request(req);
        });

        http_server.add_route("GET", "/api/torrents", [&torrent_manager](const pixelscrape::HttpRequest&) {
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

            pixellib::core::json::StringifyOptions options;
            options.pretty = true;
            response.body = torrent_array.stringify(options);
            return response;
        });

        http_server.add_route("POST", "/api/torrents", [&torrent_manager](const pixelscrape::HttpRequest& req) {
            pixelscrape::HttpResponse response;
            response.headers["Content-Type"] = "application/json";
            response.headers["Access-Control-Allow-Origin"] = "*";

            try {
                auto json_value = pixellib::core::json::JSON::parse_or_throw(req.body);
                if (!json_value.is_object()) {
                    throw std::runtime_error("Invalid JSON");
                }

                std::string torrent_id;
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

                auto metainfo_it = json_value.find("metainfo");
                auto path_it = json_value.find("path");

                if (metainfo_it && metainfo_it->is_string()) {
                    // Simple base64 decode
                    static const std::string base64_chars = 
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                        "abcdefghijklmnopqrstuvwxyz"
                        "0123456789+/";
                    
                    auto decode = [](const std::string& in) {
                        std::string out;
                        std::vector<int> T(256, -1);
                        for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;
                        int val = 0, valb = -8;
                        for (unsigned char c : in) {
                            if (T[c] == -1) continue;
                            val = (val << 6) + T[c];
                            valb += 6;
                            if (valb >= 0) {
                                out.push_back(char((val >> valb) & 0xFF));
                                valb -= 8;
                            }
                        }
                        return out;
                    };

                    std::string decoded_data = decode(metainfo_it->as_string());
                    torrent_id = torrent_manager.add_torrent_data(decoded_data, file_priorities);
                } else if (path_it && path_it->is_string()) {
                    torrent_id = torrent_manager.add_torrent(path_it->as_string(), file_priorities);
                } else {
                    throw std::runtime_error("Missing torrent path or metainfo");
                }

                pixellib::core::json::JSON result = pixellib::core::json::JSON::object({});
                result["success"] = pixellib::core::json::JSON(true);
                result["torrent_id"] = pixellib::core::json::JSON(torrent_id);
                
                pixellib::core::json::StringifyOptions options;
                options.pretty = true;
                response.body = result.stringify(options);

            } catch (const std::exception& e) {
                response.status_code = 400;
                pixellib::core::json::JSON error = pixellib::core::json::JSON::object({});
                error["success"] = pixellib::core::json::JSON(false);
                error["error"] = pixellib::core::json::JSON(e.what());
                
                pixellib::core::json::StringifyOptions options;
                options.pretty = true;
                response.body = error.stringify(options);
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
                
                pixellib::core::json::StringifyOptions options;
                options.pretty = true;
                response.body = result.stringify(options);

                if (!success) {
                    response.status_code = 404;
                }
            } else {
                response.status_code = 400;
            }

            return response;
        });

        http_server.add_route("GET", "/api/stats", [&torrent_manager](const pixelscrape::HttpRequest&) {
            pixelscrape::HttpResponse response;
            response.headers["Content-Type"] = "application/json";
            response.headers["Access-Control-Allow-Origin"] = "*";

            auto stats = torrent_manager.get_global_stats();
            pixellib::core::json::StringifyOptions options;
            options.pretty = true;
            response.body = stats.stringify(options);
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

            // Broadcast status update
            pixellib::core::json::JSON update = pixellib::core::json::JSON::object({});
            update["stats"] = torrent_manager.get_global_stats();
            
            auto torrents = torrent_manager.list_torrents();
            pixellib::core::json::JSON torrent_array = pixellib::core::json::JSON::array({});
            for (const auto& id : torrents) {
                auto status = torrent_manager.get_torrent_status(id);
                if (status) {
                    torrent_array.push_back(*status);
                }
            }
            update["torrents"] = torrent_array;

            pixellib::core::json::StringifyOptions ws_options;
            ws_options.pretty = false;
            ws_server.broadcast_text(update.stringify(ws_options));
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