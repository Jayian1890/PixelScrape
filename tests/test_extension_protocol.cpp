#include "doctest/doctest.h"
#include "extension_protocol.hpp"
#include "bencode_parser.hpp"
#include <iostream>
#include <array>

using namespace pixelscrape;

TEST_CASE("ExtensionProtocol Handshake") {
    ExtensionProtocol protocol;

    // Test build_extended_handshake
    auto payload = protocol.build_extended_handshake(6881, "PixelScrape/1.0", 12345);

    // Verify it parses back correctly
    ExtensionProtocol peer_protocol;
    CHECK(peer_protocol.parse_extended_handshake(payload));

    CHECK(peer_protocol.get_peer_client() == "PixelScrape/1.0");
    CHECK(peer_protocol.get_peer_port() == 6881);
    CHECK(peer_protocol.get_peer_metadata_size() == 12345);

    // Check supported extensions
    CHECK(peer_protocol.supports_ut_metadata());
    CHECK(peer_protocol.supports_ut_pex());
    CHECK(peer_protocol.supports_lt_donthave());
    CHECK(peer_protocol.supports_upload_only());
}

TEST_CASE("ExtensionProtocol Metadata") {
    ExtensionProtocol protocol;

    // Test Request
    auto req = protocol.build_metadata_request(1, 5);
    ExtensionProtocol::MetadataMessage msg;

    // Payload includes ext_msg_id (1 byte) + bencoded dict
    std::vector<uint8_t> req_payload(req.begin() + 1, req.end());

    CHECK(protocol.parse_metadata_message(req_payload, msg));
    CHECK(msg.msg_type == 0); // Request
    CHECK(msg.piece == 5);

    // Test Data
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
    auto resp = protocol.build_metadata_data(1, 5, 100, data);

    std::vector<uint8_t> resp_payload(resp.begin() + 1, resp.end());
    CHECK(protocol.parse_metadata_message(resp_payload, msg));
    CHECK(msg.msg_type == 1); // Data
    CHECK(msg.piece == 5);
    CHECK(msg.total_size == 100);
    CHECK(msg.data == data);

    // Test Reject
    auto reject = protocol.build_metadata_reject(1, 5);
    std::vector<uint8_t> reject_payload(reject.begin() + 1, reject.end());

    CHECK(protocol.parse_metadata_message(reject_payload, msg));
    CHECK(msg.msg_type == 2); // Reject
    CHECK(msg.piece == 5);
}

TEST_CASE("ExtensionProtocol PEX") {
    ExtensionProtocol protocol;

    std::vector<std::array<uint8_t, 6>> added;
    std::array<uint8_t, 6> peer1 = {192, 168, 1, 1, 0x1A, 0xE1}; // 192.168.1.1:6881
    added.push_back(peer1);

    std::vector<std::array<uint8_t, 6>> dropped;
    std::array<uint8_t, 6> peer2 = {10, 0, 0, 1, 0x1F, 0x90}; // 10.0.0.1:8080
    dropped.push_back(peer2);

    auto pex = protocol.build_pex_message(1, added, dropped);

    std::vector<uint8_t> pex_payload(pex.begin() + 1, pex.end());

    ExtensionProtocol::PexMessage msg;
    CHECK(protocol.parse_pex_message(pex_payload, msg));

    CHECK(msg.added.size() == 1);
    CHECK(msg.added[0] == peer1);

    CHECK(msg.dropped.size() == 1);
    CHECK(msg.dropped[0] == peer2);
}
