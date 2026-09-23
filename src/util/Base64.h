#pragma once

#include <cstddef>
#include <string>

namespace rpfg {

// Standard base64 with padding. Used to move rendered PNGs across the
// WebView2 message bridge, which only carries text.
std::string base64Encode(const unsigned char* data, std::size_t length);

}  // namespace rpfg
