#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <mupdf/fitz.h>
#include "document/Page.h"
#include "render/PageCompare.h"

namespace rpfg {

class Document;

// Low-level renderer that emits a Page's complete object tree (shapes, lines, vectors,
// images, text frames with embedded/base-14 fonts, and clipping) to any MuPDF device.
// Used for both preview rendering (draw device) and PDF export (pdf writer device).
void renderPageToDevice(fz_context* ctx, fz_device* dev, const Page& page, fz_matrix pageCtm,
                        const Document* doc, std::map<std::string, fz_font*>& fontCache);

struct RenderOptions {
    float scale = 1.0f;
    bool showCaret = false;
    Point caretPosition;
    float caretHeight = 14.0f;
    std::vector<Rect> selectionRectangles;
};

class DocumentRenderer {
public:
    DocumentRenderer();
    ~DocumentRenderer();

    // Renders a Page directly to an 8-bit grayscale image (for visual comparison)
    bool renderPageGray(const Page& page, float scale, GrayImage& out, std::string& error, const Document* doc = nullptr);

    // Renders a Page directly to 32-bit RGBA pixel buffer
    bool renderPageRgba(const Page& page, const RenderOptions& options,
                        std::vector<uint8_t>& outRgba, int& outWidth, int& outHeight,
                        std::string& error, const Document* doc = nullptr);

    // Renders a Page directly to Base64-encoded PNG (for UI Edit Mode)
    bool renderPageBase64Png(const Page& page, const RenderOptions& options,
                             std::string& outBase64, int& outWidth, int& outHeight,
                             std::string& error, const Document* doc = nullptr);
};

}  // namespace rpfg
