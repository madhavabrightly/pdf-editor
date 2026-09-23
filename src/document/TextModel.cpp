#include "document/TextModel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rpfg {
namespace {

bool isWhitespace(const std::string& text) {
    return text == " " || text == "\t" || text == "\n" || text == "\r";
}

float glyphWidth(const Glyph& glyph) {
    return glyph.advance > 0.0f ? glyph.advance : glyph.size * 0.5f;
}

}  // namespace

std::string Line::text() const {
    std::string out;
    for (const Word& word : words) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out += word.text;
    }
    return out;
}

std::string Paragraph::text() const {
    std::string out;
    for (const Line& line : lines) {
        if (!out.empty()) {
            out.push_back('\n');
        }
        out += line.text();
    }
    return out;
}

// Turns a flat glyph list into words -> lines -> paragraphs. Grouping is driven
// by tolerances scaled to the local font size, and lines are ordered by baseline
// then by x, so reading order is recovered rather than assumed.
PageModel buildPageModel(const std::vector<Glyph>& glyphs,
                         const ModelTolerances& tolerances) {
    PageModel model;
    model.glyphs = glyphs;
    if (glyphs.empty()) {
        return model;
    }

    // Order glyphs by baseline, then left-to-right. A stable sort keeps the
    // original draw order for glyphs that tie.
    std::vector<int> order(glyphs.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<int>(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Glyph& ga = glyphs[static_cast<std::size_t>(a)];
        const Glyph& gb = glyphs[static_cast<std::size_t>(b)];
        const float tolerance = tolerances.lineFactor * std::max(ga.size, gb.size);
        if (std::abs(ga.y - gb.y) > tolerance) {
            return ga.y < gb.y;
        }
        return ga.x < gb.x;
    });

    // Cluster into lines. Whitespace glyphs are kept here (they carry the line's
    // baseline) and consumed as word breaks below.
    std::vector<Line> lines;
    for (const int index : order) {
        const Glyph& glyph = glyphs[static_cast<std::size_t>(index)];
        if (glyph.text.empty()) {
            continue;
        }
        const float tolerance = tolerances.lineFactor * glyph.size;
        if (!lines.empty() && std::abs(glyph.y - lines.back().baseline) <= tolerance) {
            lines.back().size = std::max(lines.back().size, glyph.size);
        } else {
            Line line;
            line.baseline = glyph.y;
            line.size = glyph.size;
            lines.push_back(line);
        }
    }

    // Fill each line with words.
    std::vector<std::vector<int>> lineGlyphs(lines.size());
    {
        std::size_t current = 0;
        bool started = false;
        for (const int index : order) {
            const Glyph& glyph = glyphs[static_cast<std::size_t>(index)];
            if (glyph.text.empty()) {
                continue;
            }
            const float tolerance = tolerances.lineFactor * glyph.size;
            if (!started) {
                started = true;
                current = 0;
            } else if (std::abs(glyph.y - lines[current].baseline) > tolerance) {
                ++current;
            }
            if (current < lineGlyphs.size()) {
                lineGlyphs[current].push_back(index);
            }
        }
    }

    for (std::size_t li = 0; li < lines.size(); ++li) {
        std::vector<int>& members = lineGlyphs[li];
        if (members.empty()) {
            continue;
        }
        std::stable_sort(members.begin(), members.end(), [&](int a, int b) {
            return glyphs[static_cast<std::size_t>(a)].x < glyphs[static_cast<std::size_t>(b)].x;
        });

        Line& line = lines[li];
        Word word;
        float previousRight = 0.0f;
        bool haveWord = false;
        for (const int index : members) {
            const Glyph& glyph = glyphs[static_cast<std::size_t>(index)];

            // A space glyph, or a gap wider than the word tolerance, ends the
            // current word. Spaces are not carried into word text - the gap in
            // the layout represents them.
            if (isWhitespace(glyph.text)) {
                if (haveWord) {
                    line.words.push_back(word);
                    haveWord = false;
                }
                previousRight = glyph.x + glyphWidth(glyph);
                continue;
            }

            const float gap = glyph.x - previousRight;
            const bool breakHere =
                haveWord && gap > tolerances.wordGapFactor * std::max(glyph.size, word.size);
            if (breakHere) {
                line.words.push_back(word);
                word = Word{};
                haveWord = false;
            }
            if (!haveWord) {
                word = Word{};
                word.firstGlyph = index;
                word.glyphCount = 0;
                word.size = glyph.size;
                word.baseline = glyph.y;
                word.fontName = glyph.fontName;
                word.r = glyph.r;
                word.g = glyph.g;
                word.b = glyph.b;
                word.x0 = glyph.x;
                word.y0 = glyph.y - glyph.size;
                word.x1 = glyph.x;
                word.y1 = glyph.y + glyph.size * 0.25f;
                haveWord = true;
            }
            word.text += glyph.text;
            word.glyphCount += 1;
            word.size = std::max(word.size, glyph.size);
            word.x1 = std::max(word.x1, glyph.x + glyphWidth(glyph));
            word.y0 = std::min(word.y0, glyph.y - glyph.size);
            word.y1 = std::max(word.y1, glyph.y + glyph.size * 0.25f);
            previousRight = word.x1;
        }
        if (haveWord) {
            line.words.push_back(word);
        }

        if (!line.words.empty()) {
            line.x0 = line.words.front().x0;
            line.x1 = line.words.front().x1;
            line.y0 = line.words.front().y0;
            line.y1 = line.words.front().y1;
            for (const Word& w : line.words) {
                line.x0 = std::min(line.x0, w.x0);
                line.x1 = std::max(line.x1, w.x1);
                line.y0 = std::min(line.y0, w.y0);
                line.y1 = std::max(line.y1, w.y1);
            }
        }
    }

    // Drop empty lines that produced no words.
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [](const Line& line) { return line.words.empty(); }),
                lines.end());
    std::sort(lines.begin(), lines.end(),
              [](const Line& a, const Line& b) { return a.baseline < b.baseline; });

    // Group lines into paragraphs: a new paragraph on a size change, a larger
    // vertical gap than the leading, or a significant indent change.
    for (const Line& line : lines) {
        bool newParagraph = model.paragraphs.empty();
        if (!newParagraph) {
            const Paragraph& previous = model.paragraphs.back();
            const Line& previousLine = previous.lines.back();
            const float sizeDelta = std::abs(line.size - previousLine.size) /
                                    std::max(1.0f, std::max(line.size, previousLine.size));
            const float gap = line.baseline - previousLine.baseline;
            const float expectedLeading =
                previous.leading > 0.0f ? previous.leading : std::max(line.size, previousLine.size) * 1.2f;
            const float indent = std::abs(line.x0 - previous.x0);
            newParagraph = sizeDelta > tolerances.sizeFactor ||
                           gap > tolerances.paragraphGapFactor * expectedLeading ||
                           indent > tolerances.indentFactor * line.size;
        }
        if (newParagraph) {
            Paragraph paragraph;
            paragraph.size = line.size;
            paragraph.x0 = line.x0;
            paragraph.y0 = line.y0;
            paragraph.x1 = line.x1;
            paragraph.y1 = line.y1;
            paragraph.lines.push_back(line);
            model.paragraphs.push_back(std::move(paragraph));
        } else {
            Paragraph& paragraph = model.paragraphs.back();
            const float baseline = paragraph.lines.front().baseline;
            paragraph.lines.push_back(line);
            paragraph.leading = (line.baseline - baseline) /
                                static_cast<float>(paragraph.lines.size() - 1);
            paragraph.x0 = std::min(paragraph.x0, line.x0);
            paragraph.y0 = std::min(paragraph.y0, line.y0);
            paragraph.x1 = std::max(paragraph.x1, line.x1);
            paragraph.y1 = std::max(paragraph.y1, line.y1);
        }
    }

    return model;
}

}  // namespace rpfg
