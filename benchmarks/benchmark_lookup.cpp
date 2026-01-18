#include <benchmark/benchmark.h>
#include <unordered_map>
#include <vector>
#include <array>
#include <string>
#include <memory>
#include <algorithm>
#include <random>

// Mock structures to simulate the TorrentManager internals
struct TorrentMetadata {
    std::array<uint8_t, 20> info_hash;
};

struct Torrent {
    TorrentMetadata metadata;
};

struct TorrentManagerMock {
    std::unordered_map<std::string, std::unique_ptr<Torrent>> torrents_;
    std::unordered_map<std::string, std::string> info_hash_to_id_;

    void add_torrent(const std::string& id, const std::array<uint8_t, 20>& info_hash) {
        auto t = std::make_unique<Torrent>();
        t->metadata.info_hash = info_hash;
        torrents_[id] = std::move(t);

        // Populate the secondary map (for the optimized case)
        std::string hash_str(reinterpret_cast<const char*>(info_hash.data()), 20);
        info_hash_to_id_[hash_str] = id;
    }

    std::string find_linear(const std::array<uint8_t, 20>& remote_info_hash) {
        for (const auto &p : torrents_) {
            if (p.second->metadata.info_hash == remote_info_hash) {
                return p.first;
            }
        }
        return "";
    }

    std::string find_map(const std::array<uint8_t, 20>& remote_info_hash) {
        std::string hash_str(reinterpret_cast<const char*>(remote_info_hash.data()), 20);
        auto it = info_hash_to_id_.find(hash_str);
        if (it != info_hash_to_id_.end()) {
            return it->second;
        }
        return "";
    }
};

static void BM_LinearScan(benchmark::State& state) {
    TorrentManagerMock manager;
    int num_torrents = state.range(0);
    std::vector<std::array<uint8_t, 20>> hashes;

    // Setup
    for (int i = 0; i < num_torrents; ++i) {
        std::array<uint8_t, 20> hash;
        for (int j=0; j<20; ++j) hash[j] = (uint8_t)(i + j);
        manager.add_torrent(std::to_string(i), hash);
        hashes.push_back(hash);
    }

    // Worst case: search for the last added torrent (or one that doesn't exist, but let's do last added)
    // Actually, std::unordered_map order is not guaranteed, so let's pick a random one.
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, num_torrents - 1);

    for (auto _ : state) {
        int idx = dist(rng);
        benchmark::DoNotOptimize(manager.find_linear(hashes[idx]));
    }
}

static void BM_MapLookup(benchmark::State& state) {
    TorrentManagerMock manager;
    int num_torrents = state.range(0);
    std::vector<std::array<uint8_t, 20>> hashes;

    // Setup
    for (int i = 0; i < num_torrents; ++i) {
        std::array<uint8_t, 20> hash;
        for (int j=0; j<20; ++j) hash[j] = (uint8_t)(i + j);
        manager.add_torrent(std::to_string(i), hash);
        hashes.push_back(hash);
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, num_torrents - 1);

    for (auto _ : state) {
        int idx = dist(rng);
        benchmark::DoNotOptimize(manager.find_map(hashes[idx]));
    }
}

BENCHMARK(BM_LinearScan)->Range(10, 1000);
BENCHMARK(BM_MapLookup)->Range(10, 1000);

BENCHMARK_MAIN();
