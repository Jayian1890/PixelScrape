#include "manager.hpp"

namespace pixelscrape {

std::array<uint8_t, 20> TorrentManager::generate_peer_id() {
  std::array<uint8_t, 20> peer_id;
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);

  // BitTorrent peer ID format: -PS0001-XXXXXXXXXXXX
  peer_id[0] = '-';
  peer_id[1] = 'P';
  peer_id[2] = 'S';
  peer_id[3] = '0';
  peer_id[4] = '0';
  peer_id[5] = '0';
  peer_id[6] = '1';
  peer_id[7] = '-';

  for (size_t i = 8; i < 20; ++i) {
    peer_id[i] = dis(gen);
  }

  return peer_id;
}

} // namespace pixelscrape
