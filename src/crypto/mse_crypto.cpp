#include "mse_crypto.hpp"
#include <cstring>
#include <stdexcept>

namespace pixelscrape {

MSECrypto::MSECrypto()
    : dh_(nullptr, DH_free),
      encrypt_ctx_(nullptr, EVP_CIPHER_CTX_free),
      decrypt_ctx_(nullptr, EVP_CIPHER_CTX_free) {}

MSECrypto::~MSECrypto() = default;

void MSECrypto::generate_dh_keypair() {
    // Create DH structure
    DH* dh_raw = DH_new();
    if (!dh_raw) {
        throw std::runtime_error("Failed to create DH structure");
    }
    dh_.reset(dh_raw);

    // Set P and G parameters from MSE specification
    BIGNUM* p = BN_new();
    BIGNUM* g = BN_new();
    
    if (!BN_hex2bn(&p, P_HEX)) {
        BN_free(p);
        BN_free(g);
        throw std::runtime_error("Failed to set DH prime P");
    }
    
    if (!BN_set_word(g, G)) {
        BN_free(p);
        BN_free(g);
        throw std::runtime_error("Failed to set DH generator G");
    }

    // Set parameters (OpenSSL 3.0+ API)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    if (DH_set0_pqg(dh_.get(), p, nullptr, g) != 1) {
        BN_free(p);
        BN_free(g);
        throw std::runtime_error("Failed to set DH parameters");
    }
#else
    dh_->p = p;
    dh_->g = g;
#endif

    // Generate keypair
    if (DH_generate_key(dh_.get()) != 1) {
        throw std::runtime_error("Failed to generate DH keypair");
    }
}

std::vector<uint8_t> MSECrypto::get_public_key() const {
    if (!dh_) {
        throw std::runtime_error("DH not initialized");
    }

    const BIGNUM* pub_key = nullptr;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    DH_get0_key(dh_.get(), &pub_key, nullptr);
#else
    pub_key = dh_->pub_key;
#endif

    if (!pub_key) {
        throw std::runtime_error("Public key not generated");
    }

    // Public key should be 96 bytes (768 bits) padded if necessary
    std::vector<uint8_t> key(96, 0);
    int num_bytes = BN_num_bytes(pub_key);
    
    if (num_bytes > 96) {
        throw std::runtime_error("Public key too large");
    }

    // Pad with zeros at the beginning if key is shorter than 96 bytes
    int offset = 96 - num_bytes;
    BN_bn2bin(pub_key, key.data() + offset);

    return key;
}

bool MSECrypto::compute_shared_secret(const std::vector<uint8_t>& peer_public_key) {
    if (!dh_) {
        return false;
    }

    if (peer_public_key.size() != 96) {
        return false;
    }

    // Convert peer public key to BIGNUM
    BIGNUM* peer_pub_bn = BN_bin2bn(peer_public_key.data(), peer_public_key.size(), nullptr);
    if (!peer_pub_bn) {
        return false;
    }

    // Compute shared secret
    shared_secret_.resize(DH_size(dh_.get()));
    int secret_len = DH_compute_key(shared_secret_.data(), peer_pub_bn, dh_.get());
    BN_free(peer_pub_bn);

    if (secret_len < 0) {
        return false;
    }

    shared_secret_.resize(secret_len);
    return true;
}

void MSECrypto::init_rc4_encryption(const std::vector<uint8_t>& key) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create encryption context");
    }
    encrypt_ctx_.reset(ctx);

    // Initialize RC4
    if (EVP_EncryptInit_ex(encrypt_ctx_.get(), EVP_rc4(), nullptr, key.data(), nullptr) != 1) {
        throw std::runtime_error("Failed to initialize RC4 encryption");
    }
}

void MSECrypto::init_rc4_decryption(const std::vector<uint8_t>& key) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create decryption context");
    }
    decrypt_ctx_.reset(ctx);

    // Initialize RC4
    if (EVP_DecryptInit_ex(decrypt_ctx_.get(), EVP_rc4(), nullptr, key.data(), nullptr) != 1) {
        throw std::runtime_error("Failed to initialize RC4 decryption");
    }
}

void MSECrypto::encrypt(std::vector<uint8_t>& data) {
    if (!encrypt_ctx_) {
        return;
    }

    int out_len;
    std::vector<uint8_t> out(data.size() + EVP_CIPHER_CTX_block_size(encrypt_ctx_.get()));
    
    if (EVP_EncryptUpdate(encrypt_ctx_.get(), out.data(), &out_len, data.data(), data.size()) != 1) {
        return;
    }

    data.assign(out.begin(), out.begin() + out_len);
}

void MSECrypto::decrypt(std::vector<uint8_t>& data) {
    if (!decrypt_ctx_) {
        return;
    }

    int out_len;
    std::vector<uint8_t> out(data.size() + EVP_CIPHER_CTX_block_size(decrypt_ctx_.get()));
    
    if (EVP_DecryptUpdate(decrypt_ctx_.get(), out.data(), &out_len, data.data(), data.size()) != 1) {
        return;
    }

    data.assign(out.begin(), out.begin() + out_len);
}

std::array<uint8_t, 20> MSECrypto::sha1(const std::vector<uint8_t>& data) {
    std::array<uint8_t, 20> hash;
    SHA1(data.data(), data.size(), hash.data());
    return hash;
}

std::vector<uint8_t> MSECrypto::generate_random_bytes(size_t length) {
    std::vector<uint8_t> bytes(length);
    if (RAND_bytes(bytes.data(), length) != 1) {
        throw std::runtime_error("Failed to generate random bytes");
    }
    return bytes;
}

std::vector<uint8_t> MSECrypto::compute_crypto_provide(
    const std::array<uint8_t, 20>& info_hash,
    const std::vector<uint8_t>& shared_secret) const {
    
    // HASH('req1', S)
    std::vector<uint8_t> data;
    data.insert(data.end(), {'r', 'e', 'q', '1'});
    data.insert(data.end(), shared_secret.begin(), shared_secret.end());
    
    auto hash1 = sha1(data);

    // HASH('req2', SKEY) xor HASH('req3', S)
    std::vector<uint8_t> data2;
    data2.insert(data2.end(), {'r', 'e', 'q', '2'});
    data2.insert(data2.end(), info_hash.begin(), info_hash.end());
    auto hash2 = sha1(data2);

    std::vector<uint8_t> data3;
    data3.insert(data3.end(), {'r', 'e', 'q', '3'});
    data3.insert(data3.end(), shared_secret.begin(), shared_secret.end());
    auto hash3 = sha1(data3);

    std::array<uint8_t, 20> xor_hash;
    for (size_t i = 0; i < 20; ++i) {
        xor_hash[i] = hash2[i] ^ hash3[i];
    }

    // Combine: HASH('req1', S) + HASH('req2', SKEY) xor HASH('req3', S)
    std::vector<uint8_t> result;
    result.insert(result.end(), hash1.begin(), hash1.end());
    result.insert(result.end(), xor_hash.begin(), xor_hash.end());

    return result;
}

std::vector<uint8_t> MSECrypto::compute_crypto_select(
    const std::array<uint8_t, 20>& info_hash,
    const std::vector<uint8_t>& shared_secret,
    uint32_t crypto_select) const {
    
    // HASH('req1', S)
    std::vector<uint8_t> data;
    data.insert(data.end(), {'r', 'e', 'q', '1'});
    data.insert(data.end(), shared_secret.begin(), shared_secret.end());
    auto hash1 = sha1(data);

    // HASH('req2', SKEY) xor HASH('req3', S)
    std::vector<uint8_t> data2;
    data2.insert(data2.end(), {'r', 'e', 'q', '2'});
    data2.insert(data2.end(), info_hash.begin(), info_hash.end());
    auto hash2 = sha1(data2);

    std::vector<uint8_t> data3;
    data3.insert(data3.end(), {'r', 'e', 'q', '3'});
    data3.insert(data3.end(), shared_secret.begin(), shared_secret.end());
    auto hash3 = sha1(data3);

    std::array<uint8_t, 20> xor_hash;
    for (size_t i = 0; i < 20; ++i) {
        xor_hash[i] = hash2[i] ^ hash3[i];
    }

    // Result
    std::vector<uint8_t> result;
    result.insert(result.end(), hash1.begin(), hash1.end());
    result.insert(result.end(), xor_hash.begin(), xor_hash.end());
    
    // Add crypto_select (4 bytes, big-endian)
    uint32_t cs_be = htonl(crypto_select);
    uint8_t* cs_ptr = reinterpret_cast<uint8_t*>(&cs_be);
    result.insert(result.end(), cs_ptr, cs_ptr + 4);

    return result;
}

// RC4 Implementation
RC4::RC4() : i_(0), j_(0) {
    for (int i = 0; i < 256; ++i) {
        S_[i] = i;
    }
}

void RC4::init(const std::vector<uint8_t>& key) {
    // Key-scheduling algorithm (KSA)
    for (int i = 0; i < 256; ++i) {
        S_[i] = i;
    }

    uint8_t j = 0;
    for (int i = 0; i < 256; ++i) {
        j = j + S_[i] + key[i % key.size()];
        std::swap(S_[i], S_[j]);
    }

    i_ = 0;
    j_ = 0;
}

void RC4::process(std::vector<uint8_t>& data) {
    // Pseudo-random generation algorithm (PRGA)
    for (size_t k = 0; k < data.size(); ++k) {
        i_ = (i_ + 1) % 256;
        j_ = (j_ + S_[i_]) % 256;
        std::swap(S_[i_], S_[j_]);
        uint8_t K = S_[(S_[i_] + S_[j_]) % 256];
        data[k] ^= K;
    }
}

} // namespace pixelscrape
