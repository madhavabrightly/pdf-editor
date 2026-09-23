#include "util/Base64.h"

namespace rpfg {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string base64Encode(const unsigned char* data, std::size_t length) {
    std::string out;
    if (data == nullptr || length == 0) {
        return out;
    }

    out.reserve(((length + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 3 <= length; i += 3) {
        const unsigned int chunk = (static_cast<unsigned int>(data[i]) << 16) |
                                   (static_cast<unsigned int>(data[i + 1]) << 8) |
                                   static_cast<unsigned int>(data[i + 2]);
        out.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
        out.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
        out.push_back(kAlphabet[chunk & 0x3F]);
    }

    const std::size_t remaining = length - i;
    if (remaining == 1) {
        const unsigned int chunk = static_cast<unsigned int>(data[i]) << 16;
        out.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remaining == 2) {
        const unsigned int chunk =
            (static_cast<unsigned int>(data[i]) << 16) | (static_cast<unsigned int>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
        out.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

}  // namespace rpfg
