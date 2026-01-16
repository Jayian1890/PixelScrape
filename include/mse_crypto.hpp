#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <openssl/bn.h>
#include <openssl/dh.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace pixelscrape {

/**
 * Message Stream Encryption (MSE) / Protocol Encryption (PE)
 * Implementation of BEP 0008 for obfuscating BitTorrent traffic.
 */
class MSECrypto {
public:
    MSECrypto();
    ~MSECrypto();

    // Diffie-Hellman key exchange
    void generate_dh_keypair();
    std::vector<uint8_t> get_public_key() const;
    bool compute_shared_secret(const std::vector<uint8_t>& peer_public_key);

    // Encryption/Decryption using RC4
    void init_rc4_encryption(const std::vector<uint8_t>& key);
    void init_rc4_decryption(const std::vector<uint8_t>& key);
    void encrypt(std::vector<uint8_t>& data);
    void decrypt(std::vector<uint8_t>& data);

    // Hash functions for MSE protocol
    static std::array<uint8_t, 20> sha1(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> generate_random_bytes(size_t length);

    // MSE protocol helpers
    std::vector<uint8_t> compute_crypto_provide(
        const std::array<uint8_t, 20>& info_hash,
        const std::vector<uint8_t>& shared_secret) const;
    
    std::vector<uint8_t> compute_crypto_select(
        const std::array<uint8_t, 20>& info_hash,
        const std::vector<uint8_t>& shared_secret,
        uint32_t crypto_select) const;

    const std::vector<uint8_t>& get_shared_secret() const { return shared_secret_; }

private:
    // Diffie-Hellman context
    std::unique_ptr<DH, decltype(&EVP_PKEY_free)> dh_;
    std::vector<uint8_t> shared_secret_;

    // RC4 cipher contexts
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> encrypt_ctx_;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> decrypt_ctx_;

    // DH parameters (defined in MSE spec)
    static constexpr const char* P_HEX = 
        "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
        "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
        "4FE1356D6D51C245E485B576625E7EC6F44C42E9A63A36210000000000090563";
    static constexpr int G = 2;
};

/**
 * RC4 stream cipher implementation
 */
class RC4 {
public:
    RC4();
    void init(const std::vector<uint8_t>& key);
    void process(std::vector<uint8_t>& data);

private:
    std::array<uint8_t, 256> S_;
    uint8_t i_;
    uint8_t j_;
};

} // namespace pixelscrape
