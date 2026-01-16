#pragma once
#include "http_server.hpp"
#include "torrent_manager.hpp"
#include <json.hpp>

namespace pixelscrape {

class TransmissionRpcHandler {
public:
    TransmissionRpcHandler(TorrentManager* torrent_manager);
    HttpResponse handle_request(const HttpRequest& req);

private:
    TorrentManager* torrent_manager_;
    
    HttpResponse torrent_get(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag);
    HttpResponse torrent_add(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag);
    HttpResponse torrent_remove(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag);
    HttpResponse torrent_start(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag);
    HttpResponse torrent_stop(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag);
    HttpResponse session_get(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag);
    
    HttpResponse create_response(const std::string& result, const pixellib::core::json::JSON& arguments, const pixellib::core::json::JSON& tag);
};

} // namespace pixelscrape
