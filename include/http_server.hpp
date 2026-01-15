#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <thread>
#include <atomic>

namespace pixelscrape {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> query_params;
};

struct HttpResponse {
    int status_code;
    std::string status_message;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    HttpResponse(int code = 200, const std::string& message = "OK")
        : status_code(code), status_message(message) {}
};

class HttpServer {
public:
    HttpServer(int port);
    ~HttpServer();

    void start();
    void stop();

    using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;
    void add_route(const std::string& method, const std::string& path, RequestHandler handler);

private:
    void run();
    void handle_client(int client_socket);
    HttpRequest parse_request(int client_socket);
    void send_response(int client_socket, const HttpResponse& response);
    std::string url_decode(const std::string& encoded);

    int port_;
    int server_socket_;
    std::unordered_map<std::string, RequestHandler> routes_;
    std::thread server_thread_;
    std::atomic<bool> running_;
};

} // namespace pixelscrape