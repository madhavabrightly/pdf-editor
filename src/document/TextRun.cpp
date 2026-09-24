#include "document/TextRun.h"

#include <algorithm>

namespace rpfg {
namespace {

std::u32string utf8ToU32(const std::string& utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        char32_t cp = 0;
        std::size_t len = 1;
        if (lead < 0x80u) {
            cp = lead;
        } else if ((lead & 0xE0u) == 0xC0u && i + 1 < utf8.size()) {
            cp = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((lead & 0xF0u) == 0xE0u && i + 2 < utf8.size()) {
            cp = ((lead & 0x0Fu) << 12) |
                 ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu);
            len = 3;
        } else if (lead >= 0xF0u && i + 3 < utf8.size()) {
            cp = ((lead & 0x07u) << 18) |
                 ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(utf8[i + 3]) & 0x3Fu);
            len = 4;
        } else {
            cp = lead;
        }
        i += len;
        out.push_back(cp);
    }
    return out;
}

std::string u32ToUtf8(const std::u32string& u32) {
    std::string out;
    out.reserve(u32.size() * 2);
    for (const char32_t cp : u32) {
        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0x10FFFFu) {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

}  // namespace

TextRun::TextRun() = default;

TextRun::TextRun(std::u32string text, TextStyle style)
    : text_(std::move(text)), style_(std::move(style)) {}

TextRun::TextRun(const std::string& utf8Text, TextStyle style)
    : text_(utf8ToU32(utf8Text)), style_(std::move(style)) {}

void TextRun::setText(std::u32string text) {
    text_ = std::move(text);
}

std::string TextRun::textUtf8() const {
    return u32ToUtf8(text_);
}

void TextRun::setTextUtf8(const std::string& utf8Text) {
    text_ = utf8ToU32(utf8Text);
}

void TextRun::insert(std::size_t index, char32_t ch) {
    if (index > text_.size()) {
        index = text_.size();
    }
    text_.insert(text_.begin() + index, ch);
}

void TextRun::insert(std::size_t index, const std::u32string& str) {
    if (index > text_.size()) {
        index = text_.size();
    }
    text_.insert(index, str);
}

void TextRun::insert(std::size_t index, const std::string& utf8Str) {
    insert(index, utf8ToU32(utf8Str));
}

void TextRun::erase(std::size_t index, std::size_t count) {
    if (index < text_.size()) {
        text_.erase(index, count);
    }
}

TextRun TextRun::split(std::size_t charIndex) {
    if (charIndex >= text_.size()) {
        TextRun emptyTail(std::u32string{}, style_);
        return emptyTail;
    }
    std::u32string tailText = text_.substr(charIndex);
    text_.erase(charIndex);

    TextRun tail(std::move(tailText), style_);
    tail.originalGeometry_ = originalGeometry_;
    return tail;
}

}  // namespace rpfg
