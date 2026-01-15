#include "http_server.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <stdexcept>

namespace pixelscrape {

HttpServer::HttpServer(int port) : port_(port), server_socket_(-1), running_(false) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    if (running_) return;

    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        throw std::runtime_error("Failed to create server socket");
    }

    // Allow reuse of address
    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_socket_);
        throw std::runtime_error("Failed to bind server socket");
    }

    if (listen(server_socket_, 10) < 0) {
        close(server_socket_);
        throw std::runtime_error("Failed to listen on server socket");
    }

    running_ = true;
    server_thread_ = std::thread(&HttpServer::run, this);
}

void HttpServer::stop() {
    running_ = false;
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    if (server_socket_ >= 0) {
        close(server_socket_);
        server_socket_ = -1;
    }
}

void HttpServer::add_route(const std::string& method, const std::string& path, RequestHandler handler) {
    std::string key = method + " " + path;
    routes_[key] = handler;
}

void HttpServer::run() {
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(server_socket_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_socket < 0) {
            if (running_) {
                // Log error
            }
            continue;
        }

        // Handle client in a new thread
        std::thread(&HttpServer::handle_client, this, client_socket).detach();
    }
}

void HttpServer::handle_client(int client_socket) {
    try {
        HttpRequest request = parse_request(client_socket);

        // Find route
        std::string route_key = request.method + " " + request.path;
        auto it = routes_.find(route_key);

        HttpResponse response;
        if (it != routes_.end()) {
            response = it->second(request);
        } else {
            response.status_code = 404;
            response.status_message = "Not Found";
            response.body = "Route not found";
        }

        send_response(client_socket, response);
    } catch (const std::exception& e) {
        HttpResponse response(500, "Internal Server Error");
        response.body = e.what();
        send_response(client_socket, response);
    }

    close(client_socket);
}

HttpRequest HttpServer::parse_request(int client_socket) {
    HttpRequest request;
    std::string line;
    char buffer[1024];
    ssize_t bytes_read;

    // Read request line
    std::string request_line;
    while ((bytes_read = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
        request_line.append(buffer, bytes_read);
        size_t pos = request_line.find("\r\n");
        if (pos != std::string::npos) {
            line = request_line.substr(0, pos);
            request_line = request_line.substr(pos + 2);
            break;
        }
    }

    if (line.empty()) {
        throw std::runtime_error("Invalid request");
    }

    // Parse request line
    std::istringstream iss(line);
    iss >> request.method >> request.path;

    // Parse query parameters
    size_t query_pos = request.path.find('?');
    if (query_pos != std::string::npos) {
        std::string query_string = request.path.substr(query_pos + 1);
        request.path = request.path.substr(0, query_pos);

        // Simple query parsing
        std::istringstream query_iss(query_string);
        std::string param;
        while (std::getline(query_iss, param, '&')) {
            size_t eq_pos = param.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = url_decode(param.substr(0, eq_pos));
                std::string value = url_decode(param.substr(eq_pos + 1));
                request.query_params[key] = value;
            }
        }
    }

    // Read headers
    while ((bytes_read = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
        request_line.append(buffer, bytes_read);

        size_t pos;
        while ((pos = request_line.find("\r\n")) != std::string::npos) {
            line = request_line.substr(0, pos);
            request_line = request_line.substr(pos + 2);

            if (line.empty()) {
                // End of headers, read body if Content-Length present
                auto cl_it = request.headers.find("Content-Length");
                if (cl_it != request.headers.end()) {
                    size_t content_length = std::stoul(cl_it->second);
                    if (content_length > 0) {
                        request.body.resize(content_length);
                        size_t total_read = 0;
                        while (total_read < content_length) {
                            bytes_read = recv(client_socket, &request.body[total_read],
                                            content_length - total_read, 0);
                            if (bytes_read <= 0) break;
                            total_read += bytes_read;
                        }
                    }
                }
                return request;
            }

            // Parse header
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                std::string header_name = line.substr(0, colon_pos);
                std::string header_value = line.substr(colon_pos + 1);

                // Trim whitespace
                header_name.erase(header_name.begin(),
                    std::find_if(header_name.begin(), header_name.end(),
                        [](char c) { return !std::isspace(c); }));
                header_name.erase(std::find_if(header_name.rbegin(), header_name.rend(),
                    [](char c) { return !std::isspace(c); }).base(), header_name.end());

                header_value.erase(header_value.begin(),
                    std::find_if(header_value.begin(), header_value.end(),
                        [](char c) { return !std::isspace(c); }));
                header_value.erase(std::find_if(header_value.rbegin(), header_value.rend(),
                    [](char c) { return !std::isspace(c); }).base(), header_value.end());

                request.headers[header_name] = header_value;
            }
        }
    }

    throw std::runtime_error("Incomplete request");
}

void HttpServer::send_response(int client_socket, const HttpResponse& response) {
    std::stringstream ss;

    // Status line
    ss << "HTTP/1.1 " << response.status_code << " " << response.status_message << "\r\n";

    // Headers
    for (const auto& header : response.headers) {
        ss << header.first << ": " << header.second << "\r\n";
    }

    // Content-Length
    ss << "Content-Length: " << response.body.size() << "\r\n";

    // End headers
    ss << "\r\n";

    // Body
    ss << response.body;

    std::string response_str = ss.str();
    send(client_socket, response_str.data(), response_str.size(), 0);
}

std::string HttpServer::url_decode(const std::string& encoded) {
    std::string decoded;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%') {
            if (i + 2 < encoded.size()) {
                std::istringstream iss(encoded.substr(i + 1, 2));
                int hex;
                iss >> std::hex >> hex;
                decoded += static_cast<char>(hex);
                i += 2;
            }
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    return decoded;
}

} // namespace pixelscrape