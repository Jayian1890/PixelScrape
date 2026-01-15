#pragma once

#include "http_server.hpp"
#include <unordered_set>
#include <mutex>
#include <optional>
#include <functional>
#include <memory>
#include <atomic>

namespace pixelscrape {

struct WebSocketFrame {
    enum Opcode {
        CONTINUATION = 0x0,
        TEXT = 0x1,
        BINARY = 0x2,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA
    };

    bool fin;
    Opcode opcode;
    std::vector<uint8_t> payload;
};

class WebSocketConnection {
public:
    WebSocketConnection(int socket_fd);
    ~WebSocketConnection();

    bool send_frame(const WebSocketFrame& frame);
    std::optional<WebSocketFrame> receive_frame();
    bool is_connected() const { return connected_; }
    void close();

private:
    int socket_fd_;
    bool connected_;
    std::vector<uint8_t> buffer_;
};

class WebSocketServer {
public:
    WebSocketServer(int port);
    ~WebSocketServer();

    void start();
    void stop();

    // Broadcasting
    void broadcast_text(const std::string& message);
    void broadcast_binary(const std::vector<uint8_t>& data);

    // Connection management
    using MessageHandler = std::function<void(WebSocketConnection*, const WebSocketFrame&)>;
    void set_message_handler(MessageHandler handler);

private:
    void run();
    void handle_websocket_upgrade(const HttpRequest& request, int client_socket);
    void handle_websocket_connection(std::shared_ptr<WebSocketConnection> connection);
    std::string generate_accept_key(const std::string& key);

    int port_;
    std::unordered_set<std::shared_ptr<WebSocketConnection>> connections_;
    std::mutex connections_mutex_;
    MessageHandler message_handler_;
    std::thread server_thread_;
    std::atomic<bool> running_;
};

} // namespace pixelscrape