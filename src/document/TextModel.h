#pragma once

#include <string>
#include <vector>

// The native, authoritative text model. Nothing in the UI is trusted as layout:
// this module turns the glyphs a page actually draws into characters -> words ->
// lines -> paragraphs, using configurable tolerances derived from font size and
// transformed coordinates (never "same y means same line").
//
// Coordinates are PDF points with a top-left origin, matching the renderer.

namespace rpfg {

// One glyph exactly as the page draws it.
struct Glyph {
    float x = 0.0f;         // baseline origin, points from the left
    float y = 0.0f;         // baseline origin, points from the top
    float advance = 0.0f;   // horizontal advance, points
    float size = 0.0f;      // font size, points
    float angle = 0.0f;     // baseline angle, radians
    float r = 0.0f;         // fill colour, 0..1
    float g = 0.0f;
    float b = 0.0f;
    std::string fontName;
    std::string text;       // UTF-8 (one code point per glyph)
};

// A run of adjacent glyphs with no word break between them.
struct Word {
    std::string text;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float baseline = 0.0f;
    float size = 0.0f;
    int firstGlyph = 0;   // index into PageModel::glyphs
    int glyphCount = 0;
    std::string fontName;
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

struct Line {
    std::vector<Word> words;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float baseline = 0.0f;
    float size = 0.0f;
    std::string text() const;
};

struct StextParagraph {
    std::vector<Line> lines;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float size = 0.0f;     // first line's size
    float leading = 0.0f;  // baseline-to-baseline distance
    std::string text() const;
};

struct PageModel {
    float width = 0.0f;
    float height = 0.0f;
    std::vector<Glyph> glyphs;
    std::vector<StextParagraph> paragraphs;
};

// Tolerances are all expressed as fractions of the local font size, so they
// scale with the document rather than being hard-coded in points.
struct ModelTolerances {
    float lineFactor = 0.6f;         // glyphs within this * size share a baseline
    float wordGapFactor = 0.35f;     // origin gap above this * size starts a word
    float paragraphGapFactor = 1.6f; // gap above this * leading starts a paragraph
    float indentFactor = 1.2f;       // left-edge difference above this * size indents
    float sizeFactor = 0.18f;        // relative size change that starts a paragraph
};

PageModel buildPageModel(const std::vector<Glyph>& glyphs,
                         const ModelTolerances& tolerances = ModelTolerances{});

}  // namespace rpfg
