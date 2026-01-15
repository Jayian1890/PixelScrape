#include "transmission_rpc.hpp"

namespace pixelscrape {

TransmissionRpcHandler::TransmissionRpcHandler(TorrentManager& torrent_manager)
    : torrent_manager_(torrent_manager) {}

HttpResponse TransmissionRpcHandler::handle_request(const HttpRequest& req) {
    try {
        auto json_req = pixellib::core::json::JSON::parse_or_throw(req.body);
        
        auto method_it = json_req.find("method");
        auto tag_it = json_req.find("tag");
        auto args_it = json_req.find("arguments");
        
        pixellib::core::json::JSON tag = tag_it ? *tag_it : pixellib::core::json::JSON(0.0);
        pixellib::core::json::JSON args = args_it ? *args_it : pixellib::core::json::JSON::object({});
        
        if (!method_it || !method_it->is_string()) {
            return create_response("invalid method", pixellib::core::json::JSON::object({}), tag);
        }
        
        std::string method = method_it->as_string();
        
        if (method == "torrent-get") {
            return torrent_get(args, tag);
        } else if (method == "torrent-add") {
            return torrent_add(args, tag);
        } else if (method == "torrent-remove") {
            return torrent_remove(args, tag);
        } else if (method == "torrent-start") {
            return torrent_start(args, tag);
        } else if (method == "torrent-stop") {
            return torrent_stop(args, tag);
        } else if (method == "session-get") {
            return session_get(args, tag);
        }
        
        return create_response("method not implemented", pixellib::core::json::JSON::object({}), tag);
        
    } catch (const std::exception& e) {
        return create_response(e.what(), pixellib::core::json::JSON::object({}), pixellib::core::json::JSON(0.0));
    }
}

HttpResponse TransmissionRpcHandler::torrent_get(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag) {
    (void)args;
    auto torrent_ids = torrent_manager_.list_torrents();
    pixellib::core::json::JSON torrents_array = pixellib::core::json::JSON::array({});
    
    for (const auto& id : torrent_ids) {
        auto status = torrent_manager_.get_torrent_status(id);
        if (status) {
            pixellib::core::json::JSON t = pixellib::core::json::JSON::object({});
            t["id"] = pixellib::core::json::JSON(id);
            t["name"] = status->find("name") ? (*status)["name"] : pixellib::core::json::JSON("Unknown");
            t["totalSize"] = status->find("total_size") ? (*status)["total_size"] : pixellib::core::json::JSON(0.0);
            t["percentDone"] = status->find("completion") ? pixellib::core::json::JSON((*status)["completion"].as_number().to_double() / 100.0) : pixellib::core::json::JSON(0.0);
            t["rateDownload"] = pixellib::core::json::JSON(0.0); // Not tracked yet in TorrentManager
            t["rateUpload"] = pixellib::core::json::JSON(0.0);
            t["status"] = (*status)["paused"].as_bool() ? pixellib::core::json::JSON(0.0) : pixellib::core::json::JSON(4.0); // 4 = downloading
            t["peersGettingFromUs"] = status->find("peers") ? (*status)["peers"] : pixellib::core::json::JSON(0.0);
            t["peersSendingToUs"] = status->find("peers") ? (*status)["peers"] : pixellib::core::json::JSON(0.0);
            torrents_array.push_back(t);
        }
    }
    
    pixellib::core::json::JSON arguments = pixellib::core::json::JSON::object({});
    arguments["torrents"] = torrents_array;
    
    return create_response("success", arguments, tag);
}

HttpResponse TransmissionRpcHandler::torrent_add(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag) {
    auto filename_it = args.find("filename");
    auto metainfo_it = args.find("metainfo");
    
    if (metainfo_it && metainfo_it->is_string()) {
        try {
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
            std::string id = torrent_manager_.add_torrent_data(decoded_data);
            
            pixellib::core::json::JSON arguments = pixellib::core::json::JSON::object({});
            pixellib::core::json::JSON torrent_added = pixellib::core::json::JSON::object({});
            torrent_added["id"] = pixellib::core::json::JSON(id);
            torrent_added["name"] = pixellib::core::json::JSON(id);
            arguments["torrent-added"] = torrent_added;
            return create_response("success", arguments, tag);
        } catch (const std::exception& e) {
            return create_response(e.what(), pixellib::core::json::JSON::object({}), tag);
        }
    } else if (filename_it && filename_it->is_string()) {
        try {
            std::string id = torrent_manager_.add_torrent(filename_it->as_string());
            pixellib::core::json::JSON arguments = pixellib::core::json::JSON::object({});
            pixellib::core::json::JSON torrent_added = pixellib::core::json::JSON::object({});
            torrent_added["id"] = pixellib::core::json::JSON(id);
            torrent_added["name"] = pixellib::core::json::JSON(id);
            arguments["torrent-added"] = torrent_added;
            return create_response("success", arguments, tag);
        } catch (const std::exception& e) {
            return create_response(e.what(), pixellib::core::json::JSON::object({}), tag);
        }
    }
    
    return create_response("missing filename or metainfo", pixellib::core::json::JSON::object({}), tag);
}

HttpResponse TransmissionRpcHandler::torrent_remove(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag) {
    auto ids_it = args.find("ids");
    if (ids_it && ids_it->is_array()) {
        for (const auto& id_val : ids_it->as_array()) {
            if (id_val.is_string()) {
                torrent_manager_.remove_torrent(id_val.as_string());
            }
        }
        return create_response("success", pixellib::core::json::JSON::object({}), tag);
    }
    return create_response("invalid ids", pixellib::core::json::JSON::object({}), tag);
}

HttpResponse TransmissionRpcHandler::torrent_start(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag) {
    auto ids_it = args.find("ids");
    if (ids_it && ids_it->is_array()) {
        for (const auto& id_val : ids_it->as_array()) {
            if (id_val.is_string()) {
                torrent_manager_.resume_torrent(id_val.as_string());
            }
        }
        return create_response("success", pixellib::core::json::JSON::object({}), tag);
    }
    return create_response("invalid ids", pixellib::core::json::JSON::object({}), tag);
}

HttpResponse TransmissionRpcHandler::torrent_stop(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag) {
    auto ids_it = args.find("ids");
    if (ids_it && ids_it->is_array()) {
        for (const auto& id_val : ids_it->as_array()) {
            if (id_val.is_string()) {
                torrent_manager_.pause_torrent(id_val.as_string());
            }
        }
        return create_response("success", pixellib::core::json::JSON::object({}), tag);
    }
    return create_response("invalid ids", pixellib::core::json::JSON::object({}), tag);
}

HttpResponse TransmissionRpcHandler::session_get(const pixellib::core::json::JSON& args, const pixellib::core::json::JSON& tag) {
    (void)args;
    pixellib::core::json::JSON arguments = pixellib::core::json::JSON::object({});
    arguments["version"] = pixellib::core::json::JSON("4.0.0");
    arguments["rpc-version"] = pixellib::core::json::JSON(17.0);
    arguments["download-dir"] = pixellib::core::json::JSON("downloads");
    
    return create_response("success", arguments, tag);
}

HttpResponse TransmissionRpcHandler::create_response(const std::string& result, const pixellib::core::json::JSON& arguments, const pixellib::core::json::JSON& tag) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    
    pixellib::core::json::JSON res_obj = pixellib::core::json::JSON::object({});
    res_obj["result"] = pixellib::core::json::JSON(result);
    res_obj["arguments"] = arguments;
    res_obj["tag"] = tag;
    
    pixellib::core::json::StringifyOptions options;
    options.pretty = true;
    response.body = res_obj.stringify(options);
    return response;
}

} // namespace pixelscrape
