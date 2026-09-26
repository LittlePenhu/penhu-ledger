#include "penhu/util/uuid.hpp"

#include <random>
#include <array>
#include <cctype>
#include <cstdio>
#include <algorithm>

namespace penhu {
namespace {

/// UUID 不是密钥材料（会话 token 走 libsodium 的 CSPRNG），
/// 所以这里用 random_device 播种的 mt19937_64 足够，且不给 util 层引入加密库依赖。
std::mt19937_64& rng() {
    static std::mt19937_64 engine = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    return engine;
}

char hex_digit(unsigned v) {
    return static_cast<char>(v < 10 ? ('0' + v) : ('a' + (v - 10)));
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string new_uuid() {
    std::array<unsigned char, 16> bytes{};
    std::uniform_int_distribution<unsigned> dist(0, 255);
    for (auto& b : bytes) b = static_cast<unsigned char>(dist(rng()));

    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // variant 10xx

    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex_digit(bytes[i] >> 4));
        out.push_back(hex_digit(bytes[i] & 0x0F));
    }
    return out;
}

bool is_valid_uuid(std::string_view text) {
    if (text.size() != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
        } else if (hex_value(text[i]) < 0) {
            return false;
        }
    }
    // 版本位必须是 4（我们只生成 v4）
    return text[14] == '4';
}

std::string uuid_to_compact(std::string_view uuid) {
    std::string out;
    out.reserve(32);
    for (char c : uuid) {
        if (c != '-') out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string uuid_from_compact(std::string_view compact) {
    if (compact.size() != 32) return std::string(compact);
    std::string out;
    out.reserve(36);
    for (size_t i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) out.push_back('-');
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(compact[i]))));
    }
    return out;
}

}  // namespace penhu
