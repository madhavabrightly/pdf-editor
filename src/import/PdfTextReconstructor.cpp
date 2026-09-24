#include "import/PdfTextReconstructor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "document/Document.h"

namespace rpfg {
namespace {

std::string toLowerAscii(std::string str) {
    for (char& c : str) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return str;
}

bool detectBold(const std::string& name) {
    const std::string lower = toLowerAscii(name);
    return lower.find("bold") != std::string::npos ||
           lower.find("black") != std::string::npos ||
           lower.find("heavy") != std::string::npos ||
           lower.find("-b") != std::string::npos;
}

bool detectItalic(const std::string& name) {
    const std::string lower = toLowerAscii(name);
    return lower.find("italic") != std::string::npos ||
           lower.find("oblique") != std::string::npos ||
           lower.find("slant") != std::string::npos ||
           lower.find("-i") != std::string::npos;
}

}  // namespace

PdfTextReconstructor::PdfTextReconstructor() = default;

const TextSpanProbe* PdfTextReconstructor::findBestSpanMatch(
    const std::vector<TextSpanProbe>& probeSpans,
    float x, float y) const {
    const TextSpanProbe* best = nullptr;
    float bestDist = 4.0f;
    for (const auto& span : probeSpans) {
        const float dist = std::max(std::abs(span.x - x), std::abs(span.y - y));
        if (dist < bestDist) {
            bestDist = dist;
            best = &span;
        }
    }
    return best;
}

std::vector<std::shared_ptr<TextFrame>> PdfTextReconstructor::reconstructFrames(
    fz_context* ctx,
    fz_stext_page* stext,
    const std::vector<TextSpanProbe>& probeSpans,
    float pageWidth,
    float pageHeight,
    Document* doc) {
    (void)ctx;
    (void)pageWidth;
    (void)pageHeight;

    std::vector<std::shared_ptr<TextFrame>> frames;
    if (stext == nullptr) {
        return frames;
    }

    for (const fz_stext_block* block = stext->first_block; block != nullptr; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT) {
            continue;
        }

        auto frame = std::make_shared<TextFrame>();
        frame->setBounds(Rect{block->bbox.x0, block->bbox.y0, block->bbox.x1, block->bbox.y1});

        Paragraph currentParagraph;
        float lastBaseline = -1.0f;
        float lastSize = -1.0f;
        float estimatedLeading = 0.0f;

        for (const fz_stext_line* line = block->u.t.first_line; line != nullptr; line = line->next) {
            const fz_stext_char* firstChar = line->first_char;
            if (firstChar == nullptr) {
                continue;
            }

            const float lineSize = firstChar->size > 0.0f ? firstChar->size : 12.0f;
            const float lineBaseline = firstChar->origin.y;

            // Check if this line starts a new paragraph
            bool startNewParagraph = currentParagraph.runs().empty();
            if (!startNewParagraph && lastBaseline > 0.0f) {
                const float baselineGap = lineBaseline - lastBaseline;
                const float sizeDelta = std::abs(lineSize - lastSize);
                const float expectedLeading = estimatedLeading > 0.0f ? estimatedLeading : lastSize * 1.25f;

                if (sizeDelta > 2.0f || baselineGap > 1.75f * expectedLeading || baselineGap < 0.0f) {
                    startNewParagraph = true;
                } else if (estimatedLeading <= 0.0f && baselineGap > 0.0f) {
                    estimatedLeading = baselineGap;
                }
            }

            if (startNewParagraph && !currentParagraph.runs().empty()) {
                frame->addParagraph(std::move(currentParagraph));
                currentParagraph = Paragraph{};
                estimatedLeading = 0.0f;
            }

            lastBaseline = lineBaseline;
            lastSize = lineSize;

            // Collect characters into TextRuns with style & exact space preservation
            TextRun currentRun;
            float prevCharRight = -1.0f;
            char32_t prevChar = 0;

            for (const fz_stext_char* ch = firstChar; ch != nullptr; ch = ch->next) {
                const char32_t codePoint = static_cast<char32_t>(ch->c > 0 ? ch->c : 0);
                if (codePoint == 0) {
                    continue;
                }

                // Check for implicit space gap (Section 8: space character reconstruction from position)
                if (prevCharRight > 0.0f && prevChar != U' ' && codePoint != U' ') {
                    const float gap = ch->origin.x - prevCharRight;
                    // If gap is wider than 0.28 * font size, emit a real U+0020 space!
                    if (gap > 0.28f * ch->size) {
                        currentRun.insert(currentRun.length(), U' ');
                    }
                }

                // Extract font info and register embedded font
                std::string fontName = "Helvetica";
                FontId fontId;
                bool isBold = false;
                bool isItalic = false;

                if (ch->font != nullptr) {
                    const char* fn = fz_font_name(ctx, ch->font);
                    if (fn != nullptr && fn[0] != '\0') {
                        fontName = fn;
                        fontId = fn;
                    }
                    isBold = (ch->flags & FZ_STEXT_BOLD) != 0 || fz_font_is_bold(ctx, ch->font) != 0 || detectBold(fontName);
                    isItalic = fz_font_is_italic(ctx, ch->font) != 0 || detectItalic(fontName);

                    if (doc != nullptr && ch->font->buffer != nullptr && ch->font->buffer->len > 0) {
                        if (doc->findFont(fontId) == nullptr) {
                            DocumentFont docFont;
                            docFont.id = fontId;
                            docFont.postscriptName = fontName;
                            // Clean family name (strip subset prefix if any: "AAAAAA+Name" -> "Name")
                            std::string fam = fontName;
                            const std::size_t plus = fam.find('+');
                            if (plus != std::string::npos && plus + 1 < fam.size()) {
                                fam = fam.substr(plus + 1);
                            }
                            docFont.familyName = fam;
                            docFont.isEmbedded = true;
                            docFont.isBold = isBold;
                            docFont.isItalic = isItalic;
                            docFont.fontData.assign(ch->font->buffer->data, ch->font->buffer->data + ch->font->buffer->len);
                            doc->registerFont(std::move(docFont));
                        }
                    }
                }

                // Resolve style from probe matching
                const TextSpanProbe* probe = findBestSpanMatch(probeSpans, ch->origin.x, ch->origin.y);
                TextStyle charStyle;
                charStyle.fontSize = ch->size;
                charStyle.fontId = fontId;
                charStyle.font = fontName;
                charStyle.fontName = fontName;
                charStyle.bold = isBold;
                charStyle.italic = isItalic;
                if (probe != nullptr) {
                    charStyle.color = Color{probe->r, probe->g, probe->b, 1.0f};
                } else if (ch->argb != 0) {
                    float r = ((ch->argb >> 16) & 0xFF) / 255.0f;
                    float g = ((ch->argb >> 8) & 0xFF) / 255.0f;
                    float b = (ch->argb & 0xFF) / 255.0f;
                    charStyle.color = Color{r, g, b, 1.0f};
                } else {
                    charStyle.font = "helv";
                    charStyle.fontName = "Helvetica";
                    charStyle.color = Color::black();
                }

                // If run exists and style changed, push currentRun and start a new one
                if (!currentRun.empty() && !currentRun.style().matchesTypography(charStyle)) {
                    currentParagraph.addRun(std::move(currentRun));
                    currentRun = TextRun{};
                }

                if (currentRun.empty()) {
                    currentRun.setStyle(charStyle);

                    ImportedTextRunGeometry geom;
                    geom.originalBaseline = Point{ch->origin.x, ch->origin.y};
                    geom.originalBounds = Rect{ch->quad.ll.x, ch->quad.ul.y, ch->quad.ur.x, ch->quad.lr.y};
                    geom.fontSize = ch->size;
                    geom.font = charStyle.font;
                    geom.color = charStyle.color;
                    geom.hasOriginalGeometry = true;
                    currentRun.setOriginalGeometry(geom);
                } else {
                    ImportedTextRunGeometry geom = currentRun.originalGeometry();
                    geom.originalBounds.x0 = std::min(geom.originalBounds.x0, ch->quad.ll.x);
                    geom.originalBounds.y0 = std::min(geom.originalBounds.y0, ch->quad.ul.y);
                    geom.originalBounds.x1 = std::max(geom.originalBounds.x1, ch->quad.ur.x);
                    geom.originalBounds.y1 = std::max(geom.originalBounds.y1, ch->quad.lr.y);
                    currentRun.setOriginalGeometry(geom);
                }

                currentRun.insert(currentRun.length(), codePoint);
                prevChar = codePoint;
                prevCharRight = ch->quad.ur.x;
            }

            if (!currentRun.empty()) {
                currentParagraph.addRun(std::move(currentRun));
            }
        }

        if (!currentParagraph.runs().empty()) {
            frame->addParagraph(std::move(currentParagraph));
        }

        if (!frame->paragraphs().empty()) {
            frames.push_back(std::move(frame));
        }
    }

    return frames;
}

}  // namespace rpfg
