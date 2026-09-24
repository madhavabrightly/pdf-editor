#include "document/Paragraph.h"

#include <algorithm>

namespace rpfg {

Paragraph::Paragraph() = default;

Paragraph::Paragraph(TextRun initialRun) {
    runs_.push_back(std::move(initialRun));
}

Paragraph::Paragraph(const std::string& utf8Text, TextStyle style) {
    runs_.emplace_back(utf8Text, std::move(style));
}

void Paragraph::addRun(TextRun run) {
    if (!run.empty()) {
        runs_.push_back(std::move(run));
    }
}

void Paragraph::clear() {
    runs_.clear();
    layoutLines_.clear();
    layoutBounds_ = Rect{};
}

std::size_t Paragraph::totalCharacters() const {
    std::size_t total = 0;
    for (const auto& run : runs_) {
        total += run.length();
    }
    return total;
}

std::string Paragraph::textUtf8() const {
    std::string out;
    for (const auto& run : runs_) {
        out += run.textUtf8();
    }
    return out;
}

std::u32string Paragraph::textU32() const {
    std::u32string out;
    for (const auto& run : runs_) {
        out += run.text();
    }
    return out;
}

bool Paragraph::mapOffsetToRun(std::size_t globalOffset, std::size_t& runIndex, std::size_t& localOffset) const {
    if (runs_.empty()) {
        runIndex = 0;
        localOffset = 0;
        return false;
    }

    std::size_t accumulated = 0;
    for (std::size_t i = 0; i < runs_.size(); ++i) {
        const std::size_t runLen = runs_[i].length();
        if (globalOffset <= accumulated + runLen) {
            runIndex = i;
            localOffset = globalOffset - accumulated;
            return true;
        }
        accumulated += runLen;
    }

    runIndex = runs_.size() - 1;
    localOffset = runs_.back().length();
    return true;
}

void Paragraph::insertText(std::size_t globalOffset, const std::u32string& str) {
    if (str.empty()) {
        return;
    }
    if (runs_.empty()) {
        runs_.emplace_back(str);
        return;
    }

    std::size_t runIdx = 0;
    std::size_t localOffset = 0;
    mapOffsetToRun(globalOffset, runIdx, localOffset);
    runs_[runIdx].insert(localOffset, str);
}

void Paragraph::insertTextUtf8(std::size_t globalOffset, const std::string& utf8) {
    if (utf8.empty()) {
        return;
    }
    TextRun temp(utf8);
    insertText(globalOffset, temp.text());
}

void Paragraph::deleteText(std::size_t globalOffset, std::size_t count) {
    if (count == 0 || runs_.empty()) {
        return;
    }

    std::size_t runIdx = 0;
    std::size_t localOffset = 0;
    mapOffsetToRun(globalOffset, runIdx, localOffset);

    std::size_t remaining = count;
    while (remaining > 0 && runIdx < runs_.size()) {
        const std::size_t runLen = runs_[runIdx].length();
        const std::size_t availableInRun = runLen - localOffset;
        const std::size_t toDelete = std::min(remaining, availableInRun);

        runs_[runIdx].erase(localOffset, toDelete);
        remaining -= toDelete;

        // If run became empty and there are other runs, remove it
        if (runs_[runIdx].empty() && runs_.size() > 1) {
            runs_.erase(runs_.begin() + runIdx);
            localOffset = 0;
        } else {
            ++runIdx;
            localOffset = 0;
        }
    }
}

Paragraph Paragraph::split(std::size_t globalOffset) {
    Paragraph tail;
    tail.alignment_ = alignment_;
    tail.lineHeight_ = lineHeight_;
    tail.spaceAfter_ = spaceAfter_;

    if (runs_.empty() || globalOffset >= totalCharacters()) {
        tail.runs_.emplace_back(std::u32string{});
        return tail;
    }

    std::size_t runIdx = 0;
    std::size_t localOffset = 0;
    mapOffsetToRun(globalOffset, runIdx, localOffset);

    // Split the run where the offset falls
    TextRun tailRun = runs_[runIdx].split(localOffset);
    if (!tailRun.empty() || runIdx == runs_.size() - 1) {
        tail.runs_.push_back(std::move(tailRun));
    }

    // Move any subsequent runs to the tail paragraph
    for (std::size_t i = runIdx + 1; i < runs_.size(); ++i) {
        tail.runs_.push_back(std::move(runs_[i]));
    }
    runs_.erase(runs_.begin() + runIdx + 1, runs_.end());

    return tail;
}

void Paragraph::merge(Paragraph other) {
    for (auto& run : other.runs_) {
        if (run.empty()) continue;
        if (!runs_.empty() && runs_.back().style().matchesTypography(run.style())) {
            runs_.back().insert(runs_.back().length(), run.text());
        } else {
            runs_.push_back(std::move(run));
        }
    }
}

}  // namespace rpfg
