#pragma once

#include <memory>
#include <string>
#include <vector>
#include "document/Geometry.h"
#include "document/TextStyle.h"
#include "document/TextRun.h"

namespace rpfg {

struct LayoutRunPart {
    std::size_t runIndex = 0;
    std::size_t startChar = 0;
    std::size_t charCount = 0;
    Rect bounds;
    Point baseline;
    std::vector<float> characterAdvances;
};

struct LayoutLine {
    Rect bounds;
    Point baseline;
    std::vector<LayoutRunPart> runParts;
};

class Paragraph {
public:
    Paragraph();
    explicit Paragraph(TextRun initialRun);
    explicit Paragraph(const std::string& utf8Text, TextStyle style = TextStyle{});

    const std::vector<TextRun>& runs() const { return runs_; }
    std::vector<TextRun>& runs() { return runs_; }

    void addRun(TextRun run);
    void clear();

    std::size_t totalCharacters() const;
    std::string textUtf8() const;
    std::u32string textU32() const;

    // Formatting properties
    TextAlignment alignment() const { return alignment_; }
    void setAlignment(TextAlignment align) { alignment_ = align; }

    float lineHeight() const { return lineHeight_; }
    void setLineHeight(float lh) { lineHeight_ = lh; }

    float spaceBefore() const { return spaceBefore_; }
    void setSpaceBefore(float sb) { spaceBefore_ = sb; }

    float spaceAfter() const { return spaceAfter_; }
    void setSpaceAfter(float sa) { spaceAfter_ = sa; }

    float firstLineIndent() const { return firstLineIndent_; }
    void setFirstLineIndent(float ind) { firstLineIndent_ = ind; }

    // Text mutation at character index across runs
    void insertText(std::size_t globalOffset, const std::u32string& str);
    void insertTextUtf8(std::size_t globalOffset, const std::string& utf8);
    void deleteText(std::size_t globalOffset, std::size_t count);

    // Splits paragraph at global character offset. Tail runs/text move to returned Paragraph.
    Paragraph split(std::size_t globalOffset);

    // Merges another paragraph into the end of this one
    void merge(Paragraph other);

    // Layout representation (filled by LayoutEngine)
    const Rect& layoutBounds() const { return layoutBounds_; }
    void setLayoutBounds(const Rect& r) { layoutBounds_ = r; }

    const std::vector<LayoutLine>& layoutLines() const { return layoutLines_; }
    std::vector<LayoutLine>& layoutLines() { return layoutLines_; }
    void setLayoutLines(std::vector<LayoutLine> lines) { layoutLines_ = std::move(lines); }

    // Locates the run index and local character offset for a global paragraph offset
    bool mapOffsetToRun(std::size_t globalOffset, std::size_t& runIndex, std::size_t& localOffset) const;

private:
    std::vector<TextRun> runs_;
    TextAlignment alignment_ = TextAlignment::Left;
    float lineHeight_ = 1.2f;
    float spaceBefore_ = 0.0f;
    float spaceAfter_ = 0.0f;
    float firstLineIndent_ = 0.0f;

    Rect layoutBounds_;
    std::vector<LayoutLine> layoutLines_;
};

}  // namespace rpfg
