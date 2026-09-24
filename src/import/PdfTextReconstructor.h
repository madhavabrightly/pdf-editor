#pragma once

#include <memory>
#include <string>
#include <vector>
#include <mupdf/fitz.h>
#include "document/TextFrame.h"
#include "document/Paragraph.h"
#include "document/TextRun.h"

namespace rpfg {

class Document;

struct TextSpanProbe {
    float x = 0.0f;
    float y = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    std::string fontName;
    bool isBold = false;
    bool isItalic = false;
};

class PdfTextReconstructor {
public:
    PdfTextReconstructor();

    // Reconstructs TextFrames with Paragraphs and TextRuns from MuPDF stext and probe data
    std::vector<std::shared_ptr<TextFrame>> reconstructFrames(
        fz_context* ctx,
        fz_stext_page* stext,
        const std::vector<TextSpanProbe>& probeSpans,
        float pageWidth,
        float pageHeight,
        Document* doc = nullptr);

private:
    const TextSpanProbe* findBestSpanMatch(
        const std::vector<TextSpanProbe>& probeSpans,
        float x, float y) const;
};

}  // namespace rpfg
