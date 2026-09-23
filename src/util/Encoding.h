#pragma once

#include <string>

namespace rpfg {

// The WebView2 bridge and the Win32 layer speak UTF-16; MuPDF and the message
// payloads speak UTF-8. These are the only two places that should need to care.
std::string wideToUtf8(const std::wstring& wide);
std::wstring utf8ToWide(const std::string& utf8);

}  // namespace rpfg
