#include "io/DocumentPdfWriter.h"

#include <mupdf/fitz.h>

#include <cctype>
#include <map>
#include <string>

namespace rpfg {
namespace {

std::string lowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

// Maps a captured face name onto one of the fourteen standard PDF fonts. This
// keeps simple documents free of font embedding while still honouring family,
// weight and slant, so the exported text keeps the original typography.
std::string base14Name(const std::string& face) {
    const std::string name = lowerAscii(face);
    const bool bold = name.find("bold") != std::string::npos;
    const bool italic =
        name.find("italic") != std::string::npos || name.find("oblique") != std::string::npos;
    const bool serif = name.find("times") != std::string::npos ||
                       name.find("serif") != std::string::npos ||
                       name.find("georgia") != std::string::npos ||
                       name.find("garamond") != std::string::npos ||
                       name.find("cambria") != std::string::npos;
    const bool mono = name.find("courier") != std::string::npos ||
                      name.find("mono") != std::string::npos ||
                      name.find("consol") != std::string::npos;

    if (mono) {
        if (bold && italic) return "Courier-BoldOblique";
        if (bold) return "Courier-Bold";
        if (italic) return "Courier-Oblique";
        return "Courier";
    }
    if (serif) {
        if (bold && italic) return "Times-BoldItalic";
        if (bold) return "Times-Bold";
        if (italic) return "Times-Italic";
        return "Times-Roman";
    }
    if (bold && italic) return "Helvetica-BoldOblique";
    if (bold) return "Helvetica-Bold";
    if (italic) return "Helvetica-Oblique";
    return "Helvetica";
}

}  // namespace

bool writeDocumentPdf(const PageModel& model, const std::string& path, std::string& error) {
    if (model.width <= 0.0f || model.height <= 0.0f) {
        error = "the document has no page size";
        return false;
    }

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (ctx == nullptr) {
        error = "could not create a MuPDF context";
        return false;
    }
    fz_register_document_handlers(ctx);

    std::map<std::string, fz_font*> fonts;
    fz_document_writer* writer = nullptr;
    bool ok = false;

    fz_try(ctx) {
        writer = fz_new_document_writer(ctx, path.c_str(), "pdf", "compress");
        const fz_rect mediabox = fz_make_rect(0.0f, 0.0f, model.width, model.height);
        fz_device* device = fz_begin_page(ctx, writer, mediabox);
        fz_colorspace* rgb = fz_device_rgb(ctx);

        for (const Paragraph& paragraph : model.paragraphs) {
            for (const Line& line : paragraph.lines) {
                if (line.words.empty()) {
                    continue;
                }
                // Emit the whole line so the space characters are real glyphs in
                // the output - that is what keeps the text searchable and lets a
                // re-import recover the same words.
                const Word& first = line.words.front();
                const std::string key = base14Name(first.fontName);
                fz_font* font = nullptr;
                const auto found = fonts.find(key);
                if (found != fonts.end()) {
                    font = found->second;
                } else {
                    font = fz_new_base14_font(ctx, key.c_str());
                    fonts.emplace(key, font);
                }
                if (font == nullptr) {
                    continue;
                }

                const float size = line.size > 0.0f ? line.size : first.size;
                // The document model and MuPDF's document writers share the same
                // page space (top-left origin, y down), so the baseline is used
                // directly - no flip.
                const fz_matrix trm =
                    fz_make_matrix(size, 0.0f, 0.0f, size, line.x0, line.baseline);
                fz_text* text = fz_new_text(ctx);
                fz_show_string(ctx, text, font, trm, line.text().c_str(), 0, 0, FZ_BIDI_UNSET,
                               static_cast<fz_text_language>(0));
                const float color[3] = {first.r, first.g, first.b};
                fz_fill_text(ctx, device, text, fz_identity, rgb, color, 1.0f,
                             fz_default_color_params);
                fz_drop_text(ctx, text);
            }
        }

        fz_end_page(ctx, writer);
        fz_close_document_writer(ctx, writer);
        writer = nullptr;
        ok = true;
    }
    fz_catch(ctx) {
        error = fz_caught_message(ctx);
        ok = false;
    }

    if (writer != nullptr) {
        fz_try(ctx) {
            fz_drop_document_writer(ctx, writer);
        }
        fz_catch(ctx) {
        }
    }
    for (auto& entry : fonts) {
        fz_drop_font(ctx, entry.second);
    }
    fz_drop_context(ctx);
    return ok;
}

}  // namespace rpfg
