#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <array>
#include <random>
#include <chrono>

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace kimp::utils {

/**
 * Cryptographic utilities for exchange authentication
 */
class Crypto {
public:
    // HMAC-SHA256
    static std::string hmac_sha256(std::string_view key, std::string_view data) {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int result_len = 0;

        HMAC(EVP_sha256(),
             key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(),
             result, &result_len);

        return to_hex(result, result_len);
    }

    // HMAC-SHA256 raw bytes
    static std::vector<uint8_t> hmac_sha256_raw(std::string_view key, std::string_view data) {
        std::vector<uint8_t> result(EVP_MAX_MD_SIZE);
        unsigned int result_len = 0;

        HMAC(EVP_sha256(),
             key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(),
             result.data(), &result_len);

        result.resize(result_len);
        return result;
    }

    // HMAC-SHA512
    static std::string hmac_sha512(std::string_view key, std::string_view data) {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int result_len = 0;

        HMAC(EVP_sha512(),
             key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(),
             result, &result_len);

        return to_hex(result, result_len);
    }

    // SHA256
    static std::string sha256(std::string_view data) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(data.data()),
               data.size(), hash);
        return to_hex(hash, SHA256_DIGEST_LENGTH);
    }

    // SHA512
    static std::string sha512(std::string_view data) {
        unsigned char hash[SHA512_DIGEST_LENGTH];
        SHA512(reinterpret_cast<const unsigned char*>(data.data()),
               data.size(), hash);
        return to_hex(hash, SHA512_DIGEST_LENGTH);
    }

    // Generate UUID v4 — zero-alloc lookup-table hex conversion
    static std::string generate_uuid() {
        static constexpr char h[] = "0123456789abcdef";
        std::array<uint8_t, 16> bytes;
        RAND_bytes(bytes.data(), bytes.size());

        // Set version to 4
        bytes[6] = (bytes[6] & 0x0F) | 0x40;
        // Set variant to 1
        bytes[8] = (bytes[8] & 0x3F) | 0x80;

        // UUID: 8-4-4-4-12 = 36 chars
        char buf[36];
        int pos = 0;
        for (int i = 0; i < 16; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) buf[pos++] = '-';
            buf[pos++] = h[bytes[i] >> 4];
            buf[pos++] = h[bytes[i] & 0x0F];
        }
        return std::string(buf, 36);
    }

    // Generate random nonce
    static std::string generate_nonce() {
        std::array<uint8_t, 16> bytes;
        RAND_bytes(bytes.data(), bytes.size());
        return to_hex(bytes.data(), bytes.size());
    }

    // Get current timestamp in milliseconds
    static int64_t timestamp_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Get current timestamp in seconds
    static int64_t timestamp_sec() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Base64 encode
    static std::string base64_encode(const std::vector<uint8_t>& data) {
        return base64_encode(data.data(), data.size());
    }

    static std::string base64_encode(const uint8_t* data, std::size_t len) {
        static const char table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string result;
        result.reserve(((len + 2) / 3) * 4);

        for (std::size_t i = 0; i < len; i += 3) {
            uint32_t n = static_cast<uint32_t>(data[i]) << 16;
            if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
            if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

            result.push_back(table[(n >> 18) & 0x3F]);
            result.push_back(table[(n >> 12) & 0x3F]);
            result.push_back(i + 1 < len ? table[(n >> 6) & 0x3F] : '=');
            result.push_back(i + 2 < len ? table[n & 0x3F] : '=');
        }

        return result;
    }

    // Base64 decode
    static std::vector<uint8_t> base64_decode(std::string_view data) {
        static const int table[] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
        };

        std::vector<uint8_t> result;
        result.reserve((data.size() / 4) * 3);

        uint32_t val = 0;
        int bits = 0;

        for (char c : data) {
            if (c == '=') break;
            if (static_cast<unsigned char>(c) >= 128) continue;
            int v = table[static_cast<unsigned char>(c)];
            if (v < 0) continue;

            val = (val << 6) | v;
            bits += 6;

            if (bits >= 8) {
                bits -= 8;
                result.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            }
        }

        return result;
    }

    // URL encode — lookup-table, no ostringstream
    static std::string url_encode(std::string_view str) {
        static constexpr char h[] = "0123456789ABCDEF";
        std::string result;
        result.reserve(str.size());
        for (unsigned char c : str) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result.push_back(static_cast<char>(c));
            } else {
                result.push_back('%');
                result.push_back(h[c >> 4]);
                result.push_back(h[c & 0x0F]);
            }
        }
        return result;
    }

private:
    // Convert bytes to hex string — lookup-table, zero-alloc
    static std::string to_hex(const unsigned char* data, std::size_t len) {
        static constexpr char h[] = "0123456789abcdef";
        std::string result;
        result.resize(len * 2);
        for (std::size_t i = 0; i < len; ++i) {
            result[i * 2]     = h[data[i] >> 4];
            result[i * 2 + 1] = h[data[i] & 0x0F];
        }
        return result;
    }
};

} // namespace kimp::utils
