#include "websocket_server.hpp"
#include "sha1.hpp"
#include <cstring>
#include <optional>
#include <random>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

namespace pixelscrape {

static std::string base64_encode(const unsigned char* data, size_t len) {
    static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    while (len--) {
        char_array_3[i++] = *(data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; i < 4; i++)
                result += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (j = 0; j < i + 1; j++)
            result += base64_chars[char_array_4[j]];

        while (i++ < 3)
            result += '=';
    }

    return result;
}

WebSocketConnection::WebSocketConnection(int socket_fd) : socket_fd_(socket_fd), connected_(true) {}

WebSocketConnection::~WebSocketConnection() {
    close();
}

bool WebSocketConnection::send_frame(const WebSocketFrame& frame) {
    if (!connected_) return false;

    std::vector<uint8_t> data;

    // First byte: FIN + opcode
    uint8_t first_byte = (frame.fin ? 0x80 : 0x00) | static_cast<uint8_t>(frame.opcode);
    data.push_back(first_byte);

    // Second byte: mask flag (0 for server) + payload length
    size_t payload_size = frame.payload.size();
    if (payload_size <= 125) {
        data.push_back(static_cast<uint8_t>(payload_size));
    } else if (payload_size <= 65535) {
        data.push_back(126);
        data.push_back((payload_size >> 8) & 0xFF);
        data.push_back(payload_size & 0xFF);
    } else {
        data.push_back(127);
        for (int i = 7; i >= 0; --i) {
            data.push_back((payload_size >> (i * 8)) & 0xFF);
        }
    }

    // Payload
    data.insert(data.end(), frame.payload.begin(), frame.payload.end());

    ssize_t sent = send(socket_fd_, data.data(), data.size(), 0);
    return sent == static_cast<ssize_t>(data.size());
}

std::optional<WebSocketFrame> WebSocketConnection::receive_frame() {
    if (!connected_) return std::nullopt;

    WebSocketFrame frame;

    // Read first byte
    uint8_t first_byte;
    ssize_t received = recv(socket_fd_, &first_byte, 1, 0);
    if (received != 1) {
        connected_ = false;
        return std::nullopt;
    }

    frame.fin = (first_byte & 0x80) != 0;
    frame.opcode = static_cast<WebSocketFrame::Opcode>(first_byte & 0x0F);

    // Read second byte
    uint8_t second_byte;
    received = recv(socket_fd_, &second_byte, 1, 0);
    if (received != 1) {
        connected_ = false;
        return std::nullopt;
    }

    bool masked = (second_byte & 0x80) != 0;
    uint64_t payload_length = second_byte & 0x7F;

    // Extended payload length
    if (payload_length == 126) {
        uint16_t length;
        received = recv(socket_fd_, &length, 2, 0);
        if (received != 2) {
            connected_ = false;
            return std::nullopt;
        }
        payload_length = ntohs(length);
    } else if (payload_length == 127) {
        received = recv(socket_fd_, &payload_length, 8, 0);
        if (received != 8) {
            connected_ = false;
            return std::nullopt;
        }
        payload_length = __builtin_bswap64(payload_length);
    }

    // Masking key
    uint8_t masking_key[4];
    if (masked) {
        received = recv(socket_fd_, masking_key, 4, 0);
        if (received != 4) {
            connected_ = false;
            return std::nullopt;
        }
    }

    // Payload
    frame.payload.resize(payload_length);
    received = recv(socket_fd_, frame.payload.data(), payload_length, 0);
    if (received != static_cast<ssize_t>(payload_length)) {
        connected_ = false;
        return std::nullopt;
    }

    // Unmask payload
    if (masked) {
        for (size_t i = 0; i < payload_length; ++i) {
            frame.payload[i] ^= masking_key[i % 4];
        }
    }

    return frame;
}

void WebSocketConnection::close() {
    if (connected_) {
        connected_ = false;
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
    }
}

WebSocketServer::WebSocketServer(int port) : port_(port), running_(false) {}

WebSocketServer::~WebSocketServer() {
    stop();
}

void WebSocketServer::start() {
    if (running_) return;

    running_ = true;
    server_thread_ = std::thread(&WebSocketServer::run, this);
}

void WebSocketServer::stop() {
    running_ = false;
    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.clear();
}

void WebSocketServer::broadcast_text(const std::string& message) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketFrame::TEXT;
    frame.payload.assign(message.begin(), message.end());

    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto it = connections_.begin(); it != connections_.end(); ) {
        if ((*it)->send_frame(frame)) {
            ++it;
        } else {
            it = connections_.erase(it);
        }
    }
}

void WebSocketServer::broadcast_binary(const std::vector<uint8_t>& data) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketFrame::BINARY;
    frame.payload = data;

    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto it = connections_.begin(); it != connections_.end(); ) {
        if ((*it)->send_frame(frame)) {
            ++it;
        } else {
            it = connections_.erase(it);
        }
    }
}

void WebSocketServer::set_message_handler(MessageHandler handler) {
    message_handler_ = handler;
}

void WebSocketServer::handle_websocket_upgrade(const HttpRequest& request, int client_socket) {
    auto key_it = request.headers.find("Sec-WebSocket-Key");
    if (key_it == request.headers.end()) {
        close(client_socket);
        return;
    }

    std::string accept_key = generate_accept_key(key_it->second);

    std::stringstream ss;
    ss << "HTTP/1.1 101 Switching Protocols\r\n";
    ss << "Upgrade: websocket\r\n";
    ss << "Connection: Upgrade\r\n";
    ss << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
    ss << "\r\n";

    std::string handshake = ss.str();
    send(client_socket, handshake.data(), handshake.size(), 0);

    auto connection = std::make_unique<WebSocketConnection>(client_socket);
    std::thread(&WebSocketServer::handle_websocket_connection, this, std::move(connection)).detach();
}

void WebSocketServer::handle_websocket_connection(std::unique_ptr<WebSocketConnection> connection) {
    WebSocketConnection* conn_ptr = connection.get();
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.insert(std::move(connection));
    }

    while (running_ && conn_ptr->is_connected()) {
        auto frame = conn_ptr->receive_frame();
        if (frame) {
            if (message_handler_) {
                message_handler_(conn_ptr, *frame);
            }
            if (frame->opcode == WebSocketFrame::CLOSE) {
                break;
            }
        } else {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        if (it->get() == conn_ptr) {
            connections_.erase(it);
            break;
        }
    }
}

void WebSocketServer::run() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd);
        return;
    }

    if (listen(server_fd, 10) < 0) {
        close(server_fd);
        return;
    }

    while (running_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        timeval timeout{0, 100000}; // 100ms
        int activity = select(server_fd + 1, &read_fds, nullptr, nullptr, &timeout);

        if (activity > 0 && FD_ISSET(server_fd, &read_fds)) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_socket = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_socket >= 0) {
                // Read handshake request manually for now because http_server_ closes it
                char buffer[4096];
                ssize_t received = recv(client_socket, buffer, sizeof(buffer), 0);
                if (received > 0) {
                    std::string request_str(buffer, received);
                    HttpRequest request;
                    
                    std::istringstream iss(request_str);
                    std::string line;
                    std::getline(iss, line);
                    if (line.back() == '\r') line.pop_back();
                    
                    std::istringstream line_iss(line);
                    line_iss >> request.method >> request.path;
                    
                    while (std::getline(iss, line) && line != "\r" && !line.empty()) {
                        if (line.back() == '\r') line.pop_back();
                        size_t colon = line.find(':');
                        if (colon != std::string::npos) {
                            std::string key = line.substr(0, colon);
                            std::string val = line.substr(colon + 1);
                            // Trim
                            val.erase(0, val.find_first_not_of(" "));
                            request.headers[key] = val;
                        }
                    }
                    
                    handle_websocket_upgrade(request, client_socket);
                } else {
                    close(client_socket);
                }
            }
        }
    }
    close(server_fd);
}

std::string WebSocketServer::generate_accept_key(const std::string& key) {
    std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magic;

    SHA1 sha1;
    sha1.update(combined);
    std::array<uint8_t, 20> hash = sha1.finalize();

    // Base64 encode
    return base64_encode(hash.data(), hash.size());
}

} // namespace pixelscrape