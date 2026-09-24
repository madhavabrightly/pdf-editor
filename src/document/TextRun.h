#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "document/Geometry.h"
#include "document/TextStyle.h"

namespace rpfg {

struct ImportedTextRunGeometry {
    Matrix originalTransform = Matrix::identity();
    Rect originalBounds;
    Point originalBaseline;
    FontId font;
    float fontSize = 0.0f;
    float characterSpacing = 0.0f;
    float wordSpacing = 0.0f;
    Color color = Color::black();
    bool hasOriginalGeometry = false;
};

class TextRun {
public:
    TextRun();
    explicit TextRun(std::u32string text, TextStyle style = TextStyle{});
    explicit TextRun(const std::string& utf8Text, TextStyle style = TextStyle{});

    // Content: UTF-32 is authoritative (each character is an actual code point)
    const std::u32string& text() const { return text_; }
    void setText(std::u32string text);

    std::string textUtf8() const;
    void setTextUtf8(const std::string& utf8Text);

    std::size_t length() const { return text_.size(); }
    bool empty() const { return text_.empty(); }

    // Character mutations (real spaces, real characters)
    void insert(std::size_t index, char32_t ch);
    void insert(std::size_t index, const std::u32string& str);
    void insert(std::size_t index, const std::string& utf8Str);
    void insertText(std::size_t index, const std::string& utf8Str) { insert(index, utf8Str); }
    void erase(std::size_t index, std::size_t count = 1);

    // Combined bounds (original if available, layout bounds otherwise)
    Rect bounds() const {
        if (originalGeometry_.hasOriginalGeometry) return originalGeometry_.originalBounds;
        return layoutBounds_;
    }

    // Splits this run at charIndex. The current run keeps [0, charIndex),
    // and returns a new TextRun containing [charIndex, end).
    TextRun split(std::size_t charIndex);

    // Style
    const TextStyle& style() const { return style_; }
    TextStyle& style() { return style_; }
    void setStyle(const TextStyle& style) { style_ = style; }

    // Original Imported Geometry (Section 12)
    const ImportedTextRunGeometry& originalGeometry() const { return originalGeometry_; }
    ImportedTextRunGeometry& originalGeometry() { return originalGeometry_; }
    void setOriginalGeometry(const ImportedTextRunGeometry& geom) { originalGeometry_ = geom; }

    // Layout Geometry (calculated by LayoutEngine)
    const Rect& layoutBounds() const { return layoutBounds_; }
    void setLayoutBounds(const Rect& bounds) { layoutBounds_ = bounds; }

    const Point& layoutBaseline() const { return layoutBaseline_; }
    void setLayoutBaseline(const Point& baseline) { layoutBaseline_ = baseline; }

    const std::vector<float>& characterAdvances() const { return characterAdvances_; }
    void setCharacterAdvances(std::vector<float> advances) { characterAdvances_ = std::move(advances); }

private:
    std::u32string text_;
    TextStyle style_;
    ImportedTextRunGeometry originalGeometry_;

    Rect layoutBounds_;
    Point layoutBaseline_;
    std::vector<float> characterAdvances_;
};

}  // namespace rpfg
