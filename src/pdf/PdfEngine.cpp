#include "pdf/PdfEngine.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// fz_try/fz_catch are setjmp/longjmp. MSVC warns (C4611) about C++ destructors
// whose lifetime spans a setjmp; here every jump is caught in the same function
// and control flow then continues normally to the end of the enclosing block, so
// destructors of objects declared outside fz_try do run. See
// docs/ARCHITECTURE.md section 6 for the rules that keep this safe.
#pragma warning(disable : 4611)

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include "util/Base64.h"
#include "util/Encoding.h"
#include "util/Logger.h"
#include "util/ThreadPool.h"

namespace rpfg {
namespace {

// Rasterised PNGs are cached as base64 (that is what crosses the message
// bridge, so encoding once per page/zoom is the cheapest option overall).
constexpr std::size_t kMaxBitmapCacheBytes = 192ull * 1024ull * 1024ull;

// Nothing wider or taller than this gets rasterised - a stray zoom level on a
// huge page would otherwise ask MuPDF for a multi-gigabyte pixmap.
constexpr int kMaxRenderDimension = 8192;

// Zoom levels are quantised to 25% steps so that nudging the zoom slider
// reuses an existing raster instead of re-rasterising a near-identical bitmap.
constexpr float kScaleQuantum = 0.25f;

int scaleSteps(float scale) {
    const float clamped = std::clamp(scale, kScaleQuantum, 12.0f);
    const float steps = std::round(clamped / kScaleQuantum);
    return static_cast<int>(std::max(1.0f, steps));
}

float scaleFromSteps(int steps) {
    return static_cast<float>(steps) * kScaleQuantum;
}

std::uint64_t bitmapKey(int page, int steps) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(page)) << 20) |
           static_cast<std::uint64_t>(steps & 0xFFFFF);
}

// MuPDF needs FZ_LOCK_MAX mutexes that are consistent across every context
// cloned from the same parent. They must be recursive.
struct LockSet {
    std::array<std::recursive_mutex, FZ_LOCK_MAX> mutexes;
};

void lockHook(void* user, int index) {
    static_cast<LockSet*>(user)->mutexes[static_cast<std::size_t>(index)].lock();
}

void unlockHook(void* user, int index) {
    static_cast<LockSet*>(user)->mutexes[static_cast<std::size_t>(index)].unlock();
}

// MuPDF reports a Unicode code point; the bridge and the DOM want UTF-8, so
// astral characters have to be folded into a 4-byte sequence here.
void appendUtf8(std::string& out, int codePoint) {
    if (codePoint <= 0) {
        return;  // NUL and negatives have no place in a text layer
    }
    const std::uint32_t cp = static_cast<std::uint32_t>(codePoint);
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

void appendJsonString(std::string& out, const std::string& text) {
    out.push_back('"');
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20u) {
                    char escape[8]{};
                    std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(ch));
                    out += escape;
                } else {
                    out.push_back(raw);
                }
                break;
        }
    }
    out.push_back('"');
}

void appendNumber(std::string& out, double value, int precision) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    out += buffer;
}

// Writes a replacement string into a PDF content stream, encoding as WinAnsi:
// code points above U+00FF become '?' (only Latin text is editable for now), and
// the three structural characters plus control bytes are escaped. Works on the
// UTF-8 the bridge delivers.
void appendEscapedText(fz_context* ctx, fz_buffer* buffer, std::string_view utf8) {
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::uint32_t codePoint = 0;
        std::size_t length = 1;
        if (lead < 0x80u) {
            codePoint = lead;
        } else if ((lead & 0xE0u) == 0xC0u && i + 1 < utf8.size()) {
            codePoint = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
            length = 2;
        } else if ((lead & 0xF0u) == 0xE0u && i + 2 < utf8.size()) {
            codePoint = ((lead & 0x0Fu) << 12) |
                        ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 6) |
                        (static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu);
            length = 3;
        } else {
            length = lead >= 0xF0u && i + 3 < utf8.size() ? 4u : 1u;
        }
        i += length;

        const auto byte = static_cast<unsigned char>(codePoint < 0x100u ? codePoint : '?');
        switch (byte) {
            case '(':
                fz_append_string(ctx, buffer, "\\(");
                break;
            case ')':
                fz_append_string(ctx, buffer, "\\)");
                break;
            case '\\':
                fz_append_string(ctx, buffer, "\\\\");
                break;
            default:
                if (byte < 0x20u || byte == 0x7Fu) {
                    char escape[8]{};
                    std::snprintf(escape, sizeof(escape), "\\%03o", static_cast<unsigned>(byte));
                    fz_append_string(ctx, buffer, escape);
                } else {
                    fz_append_byte(ctx, buffer, byte);
                }
                break;
        }
    }
}

// Appends the text of an embedded (CID / Identity-H) font as a hex string of
// two-byte glyph ids. The document's CID font is built with a ToUnicode map from
// the font's own cmap, so text drawn this way still copies and searches
// correctly.
void appendCidHexString(fz_context* ctx, fz_buffer* buffer, std::string_view utf8, fz_font* font) {
    fz_append_byte(ctx, buffer, '<');
    char hex[8]{};
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::uint32_t codePoint = 0;
        std::size_t length = 1;
        if (lead < 0x80u) {
            codePoint = lead;
        } else if ((lead & 0xE0u) == 0xC0u && i + 1 < utf8.size()) {
            codePoint = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
            length = 2;
        } else if ((lead & 0xF0u) == 0xE0u && i + 2 < utf8.size()) {
            codePoint = ((lead & 0x0Fu) << 12) |
                        ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 6) |
                        (static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu);
            length = 3;
        } else if (lead >= 0xF0u && i + 3 < utf8.size()) {
            codePoint = ((lead & 0x07u) << 18) |
                        ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 12) |
                        ((static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu) << 6) |
                        (static_cast<unsigned char>(utf8[i + 3]) & 0x3Fu);
            length = 4;
        }
        i += length;

        int glyph = font != nullptr ? fz_encode_character(ctx, font, static_cast<int>(codePoint)) : 0;
        if (glyph <= 0 && font != nullptr) {
            glyph = fz_encode_character(ctx, font, '?');
        }
        std::snprintf(hex, sizeof(hex), "%04X", static_cast<unsigned>(glyph & 0xFFFF));
        fz_append_string(ctx, buffer, hex);
    }
    fz_append_string(ctx, buffer, "> Tj\nET\nQ\n");
}

// A resource name safe to put in a content stream, derived from the font id.
void resourceNameFor(const std::string& id, char* out, std::size_t size) {
    std::size_t n = 0;
    const char* prefix = "RpfgF_";
    for (const char* p = prefix; *p != '\0' && n + 1 < size; ++p) {
        out[n++] = *p;
    }
    for (const char ch : id) {
        if (n + 1 >= size) {
            break;
        }
        out[n++] = (std::isalnum(static_cast<unsigned char>(ch)) != 0) ? ch : '_';
    }
    out[n] = '\0';
}

// Appends one complete text object. PDF user space has its origin at the
// bottom-left, so the run's top-down baseline y becomes (pageHeight - y); flipping
// the axis conjugates the run angle, which is why the linear part is [c -s s c].
void appendTextObject(fz_context* ctx, fz_buffer* buffer, const char* resourceName, float size,
                      float angle, float x, float pdfY, float r, float g, float b, bool cid,
                      std::string_view utf8, fz_font* font) {
    const float a = std::abs(angle) > 0.0005f ? angle : 0.0f;
    const float c = std::cos(a);
    const float s = std::sin(a);

    fz_append_printf(ctx, buffer, "q\n%g %g %g rg\nBT\n/%s %g Tf\n", static_cast<double>(r),
                     static_cast<double>(g), static_cast<double>(b), resourceName,
                     static_cast<double>(size));
    fz_append_printf(ctx, buffer, "%g %g %g %g %g %g Tm\n", static_cast<double>(c),
                     static_cast<double>(-s), static_cast<double>(s), static_cast<double>(c),
                     static_cast<double>(x), static_cast<double>(pdfY));
    if (cid) {
        appendCidHexString(ctx, buffer, utf8, font);
    } else {
        fz_append_string(ctx, buffer, "(");
        appendEscapedText(ctx, buffer, utf8);
        fz_append_string(ctx, buffer, ") Tj\nET\nQ\n");
    }
}

}  // namespace

namespace {

// The PDF base-14. Every viewer already has these, so nothing is embedded.
struct StandardFont {
    const char* id;
    const char* label;
    const char* base;
};
const StandardFont kStandardFonts[] = {
    {"helv", "Helvetica", "Helvetica"},
    {"helv-b", "Helvetica Bold", "Helvetica-Bold"},
    {"helv-i", "Helvetica Italic", "Helvetica-Oblique"},
    {"times", "Times (serif)", "Times-Roman"},
    {"times-b", "Times Bold", "Times-Bold"},
    {"times-i", "Times Italic", "Times-Italic"},
    {"times-bi", "Times Bold Italic", "Times-BoldItalic"},
    {"cour", "Courier", "Courier"},
    {"cour-b", "Courier Bold", "Courier-Bold"},
    {"symbol", "Symbol", "Symbol"},
    {"zapf", "ZapfDingbats", "ZapfDingbats"},
};

// TrueType/OpenType faces looked up by file name in the extra fonts folder and in
// the Windows font directory. The list is deliberately the faces people actually
// reach for in resumes, reports and on the web.
struct KnownFont {
    const char* id;
    const char* label;
    const char* file;
};
const KnownFont kKnownFonts[] = {
    {"times-new-roman", "Times New Roman", "times.ttf"},
    {"arial", "Arial", "arial.ttf"},
    {"georgia", "Georgia", "georgia.ttf"},
    {"calibri", "Calibri", "calibri.ttf"},
    {"cambria", "Cambria", "cambria.ttc"},
    {"constantia", "Constantia", "constan.ttf"},
    {"garamond", "Garamond", "gara.ttf"},
    {"book-antiqua", "Book Antiqua", "bkant.ttf"},
    {"palatino", "Palatino Linotype", "pala.ttf"},
    {"rockwell", "Rockwell", "rock.ttf"},
    {"century-gothic", "Century Gothic", "gothic.ttf"},
    {"franklin-gothic", "Franklin Gothic Book", "framd.ttf"},
    {"candara", "Candara", "candara.ttf"},
    {"corbel", "Corbel", "corbel.ttf"},
    {"consolas", "Consolas", "consola.ttf"},
    {"segoe-ui", "Segoe UI", "segoeui.ttf"},
    {"verdana", "Verdana", "verdana.ttf"},
    {"tahoma", "Tahoma", "tahoma.ttf"},
    {"trebuchet", "Trebuchet MS", "trebuc.ttf"},
    {"lucida-console", "Lucida Console", "lucon.ttf"},
};

std::string lowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

// One text span as seen by the fill_text probe: where it starts, its fill colour
// and the font it was drawn with. Used only to decorate the run list, so the
// editor can match the document's own colour and face.
struct TextProbeSpan {
    float x = 0.0f;
    float y = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    std::string fontName;
};

// A non-text object on the page - a filled or stroked path (lines, rules, table
// borders, shapes), an image, or a shade - reduced to its bounding box. This is
// what makes geometry selectable: the page is parsed into actual objects rather
// than only the text layer.
struct PageObject {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    int kind = 0;  // 0 fill path, 1 stroke path, 2 image, 3 shade
};

// A derived device (fz_device must be its first member) that carries the span and
// object sinks; MuPDF devices have no user-data field, so the extra state lives
// here.
struct TextProbeDevice {
    fz_device base;
    std::vector<TextProbeSpan>* spans;
    std::vector<PageObject>* objects;
    std::vector<Glyph>* glyphs;
};

void recordObject(fz_context* ctx, fz_device* dev, fz_rect bounds, int kind) {
    (void)ctx;
    auto* probe = reinterpret_cast<TextProbeDevice*>(dev);
    auto* objects = probe->objects;
    if (objects == nullptr || objects->size() >= 4000) {
        return;
    }
    if (!std::isfinite(bounds.x0) || !std::isfinite(bounds.y0) || !std::isfinite(bounds.x1) ||
        !std::isfinite(bounds.y1)) {
        return;
    }
    const float width = bounds.x1 - bounds.x0;
    const float height = bounds.y1 - bounds.y0;
    if (width < 1.0f && height < 1.0f) {
        return;  // specks and stray points are not worth selecting
    }
    if (width > 20000.0f || height > 20000.0f) {
        return;
    }
    objects->push_back(PageObject{bounds.x0, bounds.y0, bounds.x1, bounds.y1, kind});
}

void probeFillPath(fz_context* ctx, fz_device* dev, const fz_path* path, int evenOdd, fz_matrix ctm,
                   fz_colorspace* colorspace, const float* color, float alpha,
                   fz_color_params params) {
    (void)evenOdd;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)params;
    recordObject(ctx, dev, fz_bound_path(ctx, path, nullptr, ctm), 0);
}

void probeStrokePath(fz_context* ctx, fz_device* dev, const fz_path* path,
                     const fz_stroke_state* stroke, fz_matrix ctm, fz_colorspace* colorspace,
                     const float* color, float alpha, fz_color_params params) {
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)params;
    recordObject(ctx, dev, fz_bound_path(ctx, path, stroke, ctm), 1);
}

void probeFillImage(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha,
                    fz_color_params params) {
    (void)image;
    (void)alpha;
    (void)params;
    recordObject(ctx, dev, fz_transform_rect(fz_unit_rect, ctm), 2);
}

void probeFillImageMask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm,
                        fz_colorspace* colorspace, const float* color, float alpha,
                        fz_color_params params) {
    (void)image;
    (void)colorspace;
    (void)color;
    (void)alpha;
    (void)params;
    recordObject(ctx, dev, fz_transform_rect(fz_unit_rect, ctm), 2);
}

void probeFillShade(fz_context* ctx, fz_device* dev, fz_shade* shade, fz_matrix ctm, float alpha,
                    fz_color_params params) {
    (void)alpha;
    (void)params;
    recordObject(ctx, dev, fz_bound_shade(ctx, shade, ctm), 3);
}

// A device that records nothing but text: no pixels, no paths. Running the
// display list through it is how per-run colour and font are recovered, because
// the structured-text pass does not carry them.
void probeFillText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                   fz_colorspace* colorspace, const float* color, float alpha,
                   fz_color_params params) {
    auto* probe = reinterpret_cast<TextProbeDevice*>(dev);
    auto* spans = probe->spans;
    if (spans == nullptr || text == nullptr) {
        return;
    }

    float rgb[FZ_MAX_COLORS] = {0};
    if (colorspace != nullptr && color != nullptr) {
        fz_convert_color(ctx, colorspace, color, fz_device_rgb(ctx), rgb, nullptr, params);
    }

    for (const fz_text_span* span = text->head; span != nullptr; span = span->next) {
        if (span->len <= 0) {
            continue;
        }
        const fz_point origin =
            fz_transform_point(fz_make_point(span->items[0].x, span->items[0].y), ctm);
        TextProbeSpan captured;
        captured.x = origin.x;
        captured.y = origin.y;
        captured.r = rgb[0];
        captured.g = rgb[1];
        captured.b = rgb[2];
        const char* name = span->font != nullptr ? fz_font_name(ctx, span->font) : nullptr;
        captured.fontName = name != nullptr ? name : "";
        spans->push_back(std::move(captured));

        // Character-level data for the native model: one Glyph per drawn glyph,
        // with its own origin, advance and size.
        if (probe->glyphs != nullptr) {
            const fz_matrix matrix = fz_concat(span->trm, ctm);
            const float scale = std::sqrt(matrix.a * matrix.a + matrix.b * matrix.b);
            const float angle = std::atan2(matrix.b, matrix.a);
            for (int i = 0; i < span->len; ++i) {
                const fz_text_item& item = span->items[i];
                const fz_point point = fz_transform_point(fz_make_point(item.x, item.y), ctm);
                Glyph glyph;
                glyph.x = point.x;
                glyph.y = point.y;
                glyph.advance = item.adv * scale;
                glyph.size = scale;
                glyph.angle = angle;
                glyph.r = rgb[0];
                glyph.g = rgb[1];
                glyph.b = rgb[2];
                glyph.fontName = captured.fontName;
                appendUtf8(glyph.text, item.ucs);
                probe->glyphs->push_back(std::move(glyph));
            }
        }
    }
}

std::string windowsFontsDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
    if (length == 0) {
        return {};
    }
    std::wstring path(buffer, length);
    if (!path.empty() && path.back() != L'\\') {
        path.push_back(L'\\');
    }
    path += L"Fonts";
    const std::string utf8 = wideToUtf8(path);
    return utf8;
}

}  // namespace

struct PdfEngine::Impl {
    // ------------------------------------------------------------- contexts --
    LockSet locks;
    fz_locks_context locksContext{};
    fz_context* baseContext = nullptr;

    std::unique_ptr<ThreadPool> pool;
    std::vector<fz_context*> workerContexts;

    // ------------------------------------------------------------- document --
    mutable std::mutex documentMutex;
    fz_document* document = nullptr;
    DocumentInfo info;
    std::atomic<bool> open{false};
    std::atomic<bool> shuttingDown{false};

    // ----------------------------------------------------------- callbacks --
    std::mutex callbackMutex;
    PageReadyCallback pageReady;

    std::mutex textCallbackMutex;
    PageTextCallback pageTextReady;

    // -------------------------------------------------------- display lists --
    // Zoom independent, built once per page under documentMutex, then shared
    // by every renderer.
    struct DisplayListEntry {
        fz_display_list* list = nullptr;
        PageSize size;
    };
    std::mutex displayListMutex;
    std::unordered_map<int, DisplayListEntry> displayLists;

    // ------------------------------------------------------------ bitmaps ----
    struct BitmapEntry {
        std::shared_ptr<const RenderedPage> page;
        std::uint64_t lastUsed = 0;
    };
    mutable std::mutex bitmapMutex;
    std::unordered_map<std::uint64_t, BitmapEntry> bitmaps;
    std::size_t bitmapBytes = 0;
    std::uint64_t useClock = 0;

    // --------------------------------------------------------------- text ----
    // Keyed by page alone: extraction is zoom-independent, so zooming never
    // invalidates it and any given page is analysed at most once per document.
    mutable std::mutex textMutex;
    std::unordered_map<int, std::shared_ptr<const PageText>> textLayers;
    // The reconstructed native model per page, built alongside the text layer.
    std::unordered_map<int, std::shared_ptr<const PageModel>> textModels;

    std::mutex textInflightMutex;
    std::unordered_set<int> textInflight;

    // Pages whose text the UI currently holds. A page leaving the viewport is
    // forgotten so that scrolling back re-sends it - the UI drops text layers
    // for far-away pages to bound its DOM.
    std::unordered_set<int> textDelivered;

    // --------------------------------------------------------------- fonts --
    mutable std::mutex fontsMutex;
    std::string fontsDir;
    bool fontsBuilt = false;
    std::vector<FontInfo> fonts;
    std::unordered_map<std::string, fz_font*> embeddedFonts;   // owned, baseContext
    std::unordered_map<std::string, pdf_obj*> cidFontObjects;  // owned, document

    void buildFontList();
    const FontInfo* findFont(const std::string& id) const;
    // Best-effort: maps a PDF font name ("ABCDEF+TimesNewRomanPSMT") to an
    // available FontInfo id, so an edit can reuse the document's own face.
    std::string matchFontId(const std::string& pdfFontName);
    fz_font* acquireEmbeddedFont(fz_context* ctx, const FontInfo& info);
    pdf_obj* acquireCidFont(fz_context* ctx, pdf_document* pdoc, const FontInfo& info);
    void dropFonts(fz_context* ctx);
    void applyEditToPage(fz_context* ctx, pdf_document* pdoc, pdf_page* page, pdf_obj* pageObj,
                         const TextEdit& edit, float pageHeight);
    void applyBlockToPage(fz_context* ctx, pdf_document* pdoc, pdf_page* page, pdf_obj* pageObj,
                          const TextBlockEdit& edit, float pageHeight);
    // Ensures the page's resource dictionary has a font entry for `fontId` and
    // reports back the resource name, whether it is a CID font, and the fz_font to
    // encode with (embedded fonts only).
    void resolveFontResource(fz_context* ctx, pdf_document* pdoc, pdf_page* page, pdf_obj* pageObj,
                             const std::string& fontId, char* resourceName, std::size_t nameSize,
                             bool& cid, fz_font** embedFont);
    void attachContent(fz_context* ctx, pdf_document* pdoc, pdf_page* page, pdf_obj* pageObj,
                       pdf_obj* stream);

    // ------------------------------------------------------------ scheduling -
    std::mutex wantedMutex;
    std::unordered_set<std::uint64_t> wanted;

    std::mutex inflightMutex;
    std::unordered_set<std::uint64_t> inflight;

    // -------------------------------------------------------------- counters -
    std::atomic<std::uint64_t> jobsSubmitted{0};
    std::atomic<std::uint64_t> jobsCoalesced{0};
    std::atomic<std::uint64_t> jobsCancelled{0};
    std::atomic<std::uint64_t> pagesRasterised{0};
    std::atomic<std::uint64_t> displayListHits{0};
    std::atomic<std::uint64_t> bitmapHits{0};
    std::atomic<std::uint64_t> textLayersBuilt{0};
    std::atomic<std::uint64_t> editsApplied{0};
    std::atomic<bool> dirty{false};

    // Set once the document has been written out in full (Save As, or a fallback
    // full save). After that an incremental append would no longer line up with
    // the file on disk, so every later save rewrites the file instead.
    bool fullSaveDone = false;

    // ------------------------------------------------------------- helpers --
    bool isWanted(std::uint64_t key);
    bool markInflight(std::uint64_t key);
    void clearInflight(std::uint64_t key);
    void invalidatePage(int pageNumber);

    std::shared_ptr<const PageText> lookupText(int pageNumber);
    bool markTextInflight(int pageNumber);
    void clearTextInflight(int pageNumber);
    std::shared_ptr<const std::string> buildTextRuns(fz_context* ctx, fz_display_list* list,
                                                     int pageNumber);
    void runTextJob(int pageNumber);
    void deliverText(std::shared_ptr<const PageText> text);
    void clearText();

    std::shared_ptr<const RenderedPage> lookupBitmap(std::uint64_t key);
    void storeBitmap(std::uint64_t key, std::shared_ptr<const RenderedPage> page);
    void evictBitmapsLocked();

    fz_display_list* acquireDisplayList(fz_context* ctx, int pageNumber, PageSize& size, bool& fromCache);
    std::shared_ptr<const std::string> rasteriseToBase64Png(fz_context* ctx, fz_display_list* list,
                                                            float scale, int& outWidth, int& outHeight);

    void deliver(std::shared_ptr<const RenderedPage> page);
    void runRenderJob(int pageNumber, PageSize pageSize, float scale, std::uint64_t key);

    void clearDisplayLists();
    void clearBitmaps();
};

// ---------------------------------------------------------------- helpers ----

bool PdfEngine::Impl::isWanted(std::uint64_t key) {
    std::lock_guard<std::mutex> guard(wantedMutex);
    return wanted.find(key) != wanted.end();
}

bool PdfEngine::Impl::markInflight(std::uint64_t key) {
    std::lock_guard<std::mutex> guard(inflightMutex);
    return inflight.insert(key).second;
}

void PdfEngine::Impl::clearInflight(std::uint64_t key) {
    std::lock_guard<std::mutex> guard(inflightMutex);
    inflight.erase(key);
}

std::shared_ptr<const RenderedPage> PdfEngine::Impl::lookupBitmap(std::uint64_t key) {
    std::lock_guard<std::mutex> guard(bitmapMutex);
    auto it = bitmaps.find(key);
    if (it == bitmaps.end()) {
        return nullptr;
    }
    it->second.lastUsed = ++useClock;
    bitmapHits.fetch_add(1);
    return it->second.page;
}

void PdfEngine::Impl::storeBitmap(std::uint64_t key, std::shared_ptr<const RenderedPage> page) {
    std::lock_guard<std::mutex> guard(bitmapMutex);
    auto [it, inserted] = bitmaps.emplace(key, BitmapEntry{std::move(page), ++useClock});
    if (inserted) {
        bitmapBytes += it->second.page->base64Png ? it->second.page->base64Png->size() : 0;
    } else {
        it->second.lastUsed = ++useClock;
    }
    evictBitmapsLocked();
}

void PdfEngine::Impl::evictBitmapsLocked() {
    // Least-recently-used eviction. Page counts are small enough that a linear
    // scan beats maintaining a separate ordering structure.
    while (bitmapBytes > kMaxBitmapCacheBytes && !bitmaps.empty()) {
        auto oldest = bitmaps.begin();
        for (auto it = bitmaps.begin(); it != bitmaps.end(); ++it) {
            if (it->second.lastUsed < oldest->second.lastUsed) {
                oldest = it;
            }
        }
        if (oldest->second.page->base64Png) {
            bitmapBytes -= oldest->second.page->base64Png->size();
        }
        bitmaps.erase(oldest);
    }
}

void PdfEngine::Impl::clearDisplayLists() {
    std::unordered_map<int, DisplayListEntry> discarded;
    {
        std::lock_guard<std::mutex> guard(displayListMutex);
        discarded.swap(displayLists);
    }
    for (auto& [pageNumber, entry] : discarded) {
        (void)pageNumber;
        if (entry.list) {
            fz_drop_display_list(baseContext, entry.list);
        }
    }
}

void PdfEngine::Impl::clearBitmaps() {
    std::lock_guard<std::mutex> guard(bitmapMutex);
    bitmaps.clear();
    bitmapBytes = 0;
}

std::shared_ptr<const PageText> PdfEngine::Impl::lookupText(int pageNumber) {
    std::lock_guard<std::mutex> guard(textMutex);
    auto it = textLayers.find(pageNumber);
    return it == textLayers.end() ? nullptr : it->second;
}

bool PdfEngine::Impl::markTextInflight(int pageNumber) {
    std::lock_guard<std::mutex> guard(textInflightMutex);
    return textInflight.insert(pageNumber).second;
}

void PdfEngine::Impl::clearTextInflight(int pageNumber) {
    std::lock_guard<std::mutex> guard(textInflightMutex);
    textInflight.erase(pageNumber);
}

void PdfEngine::Impl::clearText() {
    {
        std::lock_guard<std::mutex> guard(textMutex);
        textLayers.clear();
        textModels.clear();
    }
    {
        std::lock_guard<std::mutex> guard(textInflightMutex);
        textInflight.clear();
    }
}

// Everything derived from a page that has been mutated has to go: the display
// list, every raster keyed on it, and its text layer. `textDelivered` is cleared
// too so a later viewport report re-sends the newly extracted text.
void PdfEngine::Impl::invalidatePage(int pageNumber) {
    fz_display_list* list = nullptr;
    {
        std::lock_guard<std::mutex> guard(displayListMutex);
        auto it = displayLists.find(pageNumber);
        if (it != displayLists.end()) {
            list = it->second.list;
            displayLists.erase(it);
        }
    }
    if (list != nullptr) {
        fz_drop_display_list(baseContext, list);
    }

    {
        std::lock_guard<std::mutex> guard(bitmapMutex);
        const std::uint64_t pageBits =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(pageNumber)) << 20;
        for (auto it = bitmaps.begin(); it != bitmaps.end();) {
            if ((it->first & ~static_cast<std::uint64_t>(0xFFFFF)) == pageBits) {
                if (it->second.page->base64Png) {
                    bitmapBytes -= it->second.page->base64Png->size();
                }
                it = bitmaps.erase(it);
            } else {
                ++it;
            }
        }
    }
    {
        std::lock_guard<std::mutex> guard(textMutex);
        textLayers.erase(pageNumber);
        textModels.erase(pageNumber);
    }
    {
        std::lock_guard<std::mutex> guard(wantedMutex);
        textDelivered.erase(pageNumber);
    }
    {
        std::lock_guard<std::mutex> guard(textInflightMutex);
        textInflight.erase(pageNumber);
    }
}

// ---------------------------------------------------------------- fonts ----

// Scans the extra fonts folder and the Windows font directory once, building the
// list the UI picks from. The curated faces are found by file name; anything else
// the user dropped into the folder is added too.
void PdfEngine::Impl::buildFontList() {
    std::lock_guard<std::mutex> guard(fontsMutex);
    if (fontsBuilt) {
        return;
    }
    fontsBuilt = true;

    for (const StandardFont& face : kStandardFonts) {
        FontInfo info;
        info.id = face.id;
        info.label = face.label;
        info.baseFont = face.base;
        fonts.push_back(std::move(info));
    }

    const std::string windowsFonts = windowsFontsDirectory();
    std::unordered_set<std::string> seen;

    auto findByFile = [&](const std::string& wanted) -> std::string {
        const std::string needle = lowerAscii(wanted);
        for (const std::string& dir : {fontsDir, windowsFonts}) {
            if (dir.empty()) {
                continue;
            }
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) {
                    break;
                }
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (lowerAscii(entry.path().filename().string()) == needle) {
                    return entry.path().string();
                }
            }
        }
        return {};
    };

    for (const KnownFont& face : kKnownFonts) {
        const std::string name = lowerAscii(face.file);
        if (seen.count(name) != 0) {
            continue;
        }
        const std::string path = findByFile(face.file);
        if (path.empty()) {
            continue;
        }
        FontInfo info;
        info.id = face.id;
        info.label = face.label;
        info.embedded = true;
        info.path = path;
        fonts.push_back(std::move(info));
        seen.insert(name);
    }

    if (!fontsDir.empty()) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(fontsDir, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string ext = lowerAscii(entry.path().extension().string());
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") {
                continue;
            }
            const std::string name = lowerAscii(entry.path().filename().string());
            if (seen.count(name) != 0) {
                continue;
            }
            const std::string stem = entry.path().stem().string();
            FontInfo info;
            info.id = "file-" + lowerAscii(stem);
            info.label = stem;
            info.embedded = true;
            info.path = entry.path().string();
            fonts.push_back(std::move(info));
            seen.insert(name);
        }
    }

    logInfo("font library: " + std::to_string(fonts.size()) + " faces");
}

const FontInfo* PdfEngine::Impl::findFont(const std::string& id) const {
    std::lock_guard<std::mutex> guard(fontsMutex);
    for (const FontInfo& info : fonts) {
        if (info.id == id) {
            return &info;
        }
    }
    return nullptr;
}

std::string PdfEngine::Impl::matchFontId(const std::string& pdfFontName) {
    if (pdfFontName.empty()) {
        return {};
    }
    buildFontList();

    // Normalise away the subset prefix ("ABCDEF+"), case and punctuation, so
    // "TimesNewRomanPSMT" and "Times New Roman" collapse to the same key.
    auto normalize = [](std::string value) {
        if (value.size() > 7 && value[6] == '+') {
            value = value.substr(7);
        }
        std::string out;
        for (const char ch : value) {
            if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
        }
        return out;
    };

    const std::string needle = normalize(pdfFontName);
    if (needle.empty()) {
        return {};
    }

    std::lock_guard<std::mutex> guard(fontsMutex);
    const FontInfo* best = nullptr;
    int bestScore = 0;
    std::size_t bestLength = 0;
    for (const FontInfo& info : fonts) {
        for (const std::string& candidate : {info.label, info.id, info.baseFont}) {
            const std::string key = normalize(candidate);
            if (key.empty()) {
                continue;
            }
            // Rank exact matches above prefix matches, so "Helvetica" picks the
            // regular face rather than "Helvetica Italic" (whose id happens to be
            // a prefix of the name).
            int score = 0;
            if (needle == key) {
                score = 3;
            } else if (needle.rfind(key, 0) == 0) {
                score = 2;
            } else if (key.rfind(needle, 0) == 0) {
                score = 1;
            }
            if (score == 0) {
                continue;
            }
            const bool betterKind = best != nullptr && score == bestScore &&
                                    info.embedded == best->embedded && key.size() > bestLength;
            if (best == nullptr || score > bestScore ||
                (score == bestScore && info.embedded && !best->embedded) || betterKind) {
                best = &info;
                bestScore = score;
                bestLength = key.size();
            }
        }
    }
    return best != nullptr ? best->id : std::string{};
}

fz_font* PdfEngine::Impl::acquireEmbeddedFont(fz_context* ctx, const FontInfo& info) {
    std::lock_guard<std::mutex> guard(fontsMutex);
    auto it = embeddedFonts.find(info.id);
    if (it != embeddedFonts.end()) {
        return it->second;
    }
    fz_font* font = fz_new_font_from_file(ctx, info.label.c_str(), info.path.c_str(), 0, 0);
    embeddedFonts.emplace(info.id, font);
    return font;
}

pdf_obj* PdfEngine::Impl::acquireCidFont(fz_context* ctx, pdf_document* pdoc,
                                         const FontInfo& info) {
    auto it = cidFontObjects.find(info.id);
    if (it != cidFontObjects.end()) {
        return it->second;
    }
    fz_font* font = acquireEmbeddedFont(ctx, info);
    pdf_obj* obj = pdf_add_cid_font(ctx, pdoc, font);
    cidFontObjects.emplace(info.id, obj);
    return obj;
}

void PdfEngine::Impl::dropFonts(fz_context* ctx) {
    for (auto& entry : embeddedFonts) {
        fz_drop_font(ctx, entry.second);
    }
    embeddedFonts.clear();
    for (auto& entry : cidFontObjects) {
        pdf_drop_obj(ctx, entry.second);
    }
    cidFontObjects.clear();
}

// Turns a page's display list into a compact run list. Reusing the display list
// means text extraction costs no extra document access at all - the very same
// cached object that feeds the rasteriser feeds this.
std::shared_ptr<const std::string> PdfEngine::Impl::buildTextRuns(fz_context* ctx,
                                                                 fz_display_list* list,
                                                                 int pageNumber) {
    fz_stext_page* stext = nullptr;
    fz_stext_options options{};

    fz_var(stext);
    fz_try(ctx) {
        fz_init_stext_options(ctx, &options);
        stext = fz_new_stext_page_from_display_list(ctx, list, &options);
    }
    fz_catch(ctx) {
        stext = nullptr;
    }
    if (stext == nullptr) {
        return nullptr;
    }

    // Recover per-run colour and font by replaying the same display list through a
    // text-only probe device. Geometry comes from the structured-text pass above;
    // the probe just decorates it.
    std::vector<TextProbeSpan> probeSpans;
    std::vector<PageObject> probeObjects;
    std::vector<Glyph> probeGlyphs;
    {
        auto* probe = fz_new_derived_device(ctx, TextProbeDevice);
        probe->spans = &probeSpans;
        probe->objects = &probeObjects;
        probe->glyphs = &probeGlyphs;
        probe->base.fill_text = probeFillText;
        probe->base.fill_path = probeFillPath;
        probe->base.stroke_path = probeStrokePath;
        probe->base.fill_image = probeFillImage;
        probe->base.fill_image_mask = probeFillImageMask;
        probe->base.fill_shade = probeFillShade;
        fz_try(ctx) {
            fz_run_display_list(ctx, list, &probe->base, fz_identity, fz_infinite_rect, nullptr);
        }
        fz_always(ctx) {
            fz_drop_device(ctx, &probe->base);
        }
        fz_catch(ctx) {
            probeSpans.clear();
            probeObjects.clear();
            probeGlyphs.clear();
        }
    }

    // Reconstruct the native model (characters -> words -> lines -> paragraphs)
    // from the glyphs, and cache it by page.
    {
        PageModel built = buildPageModel(probeGlyphs);
        const fz_rect bounds = fz_bound_display_list(ctx, list);
        built.width = bounds.x1 - bounds.x0;
        built.height = bounds.y1 - bounds.y0;
        std::lock_guard<std::mutex> guard(textMutex);
        textModels[pageNumber] = std::make_shared<const PageModel>(std::move(built));
    }

    // Walking the extracted structure cannot throw, so there is no fz_try here -
    // which is exactly what makes ordinary C++ safe for building the payload.
    std::string out = "{\"runs\":[";
    bool firstRun = true;

    for (const fz_stext_block* block = stext->first_block; block != nullptr; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT) {
            continue;  // images carry no selectable text
        }
        for (const fz_stext_line* line = block->u.t.first_line; line != nullptr;
             line = line->next) {
            const fz_stext_char* firstChar = line->first_char;
            if (firstChar == nullptr) {
                continue;
            }

            std::string text;
            const fz_stext_char* lastChar = firstChar;
            for (const fz_stext_char* ch = firstChar; ch != nullptr; ch = ch->next) {
                appendUtf8(text, ch->c);
                lastChar = ch;
            }
            if (text.empty()) {
                continue;
            }

            // Advance along the baseline from the first glyph origin to the end
            // of the last glyph. Projecting onto the line direction keeps this
            // correct for rotated runs as well.
            const float dx = lastChar->quad.ur.x - firstChar->origin.x;
            const float dy = lastChar->quad.ur.y - firstChar->origin.y;
            const float advance = dx * line->dir.x + dy * line->dir.y;
            const float angle = std::atan2(line->dir.y, line->dir.x);
            const float size = firstChar->size > 0.0f ? firstChar->size : 1.0f;
            const float ascender =
                firstChar->font != nullptr ? fz_font_ascender(ctx, firstChar->font) : 0.8f;

            // Match the run to its probe span by baseline origin - the two passes
            // report the same point, so a small tolerance is plenty.
            const TextProbeSpan* probed = nullptr;
            float bestDistance = 2.0f;
            for (const TextProbeSpan& candidate : probeSpans) {
                const float distance = std::max(std::abs(candidate.x - firstChar->origin.x),
                                                std::abs(candidate.y - firstChar->origin.y));
                if (distance < bestDistance) {
                    bestDistance = distance;
                    probed = &candidate;
                }
            }
            const std::string fontName = probed != nullptr ? probed->fontName : std::string();
            const std::string fontId = matchFontId(fontName);

            if (!firstRun) {
                out.push_back(',');
            }
            firstRun = false;
            out += "{\"x\":";
            appendNumber(out, firstChar->origin.x, 3);
            out += ",\"y\":";
            appendNumber(out, firstChar->origin.y, 3);
            out += ",\"s\":";
            appendNumber(out, size, 3);
            out += ",\"w\":";
            appendNumber(out, advance, 3);
            out += ",\"a\":";
            appendNumber(out, angle, 5);
            out += ",\"asc\":";
            appendNumber(out, ascender, 4);
            out += ",\"c\":[";
            appendNumber(out, probed != nullptr ? probed->r : 0.0f, 3);
            out.push_back(',');
            appendNumber(out, probed != nullptr ? probed->g : 0.0f, 3);
            out.push_back(',');
            appendNumber(out, probed != nullptr ? probed->b : 0.0f, 3);
            out += "],\"f\":";
            appendJsonString(out, fontName);
            out += ",\"fid\":";
            appendJsonString(out, fontId);
            out += ",\"t\":";
            appendJsonString(out, text);
            out.push_back('}');
        }
    }

    out += "],\"objects\":[";
    bool firstObject = true;
    for (const PageObject& object : probeObjects) {
        if (!firstObject) {
            out.push_back(',');
        }
        firstObject = false;
        out += "{\"x0\":";
        appendNumber(out, object.x0, 2);
        out += ",\"y0\":";
        appendNumber(out, object.y0, 2);
        out += ",\"x1\":";
        appendNumber(out, object.x1, 2);
        out += ",\"y1\":";
        appendNumber(out, object.y1, 2);
        out += ",\"k\":";
        out += std::to_string(object.kind);
        out.push_back('}');
    }
    out += "]}";
    fz_drop_stext_page(ctx, stext);
    return std::make_shared<const std::string>(std::move(out));
}

void PdfEngine::Impl::runTextJob(int pageNumber) {
    struct InflightRelease {
        Impl& impl;
        int page;
        ~InflightRelease() { impl.clearTextInflight(page); }
    } release{*this, pageNumber};

    const int workerIndex = ThreadPool::workerIndex();
    if (workerIndex < 0 || static_cast<std::size_t>(workerIndex) >= workerContexts.size()) {
        return;
    }
    fz_context* ctx = workerContexts[static_cast<std::size_t>(workerIndex)];
    if (ctx == nullptr || shuttingDown.load() || !open.load()) {
        return;
    }

    // Deliberately never cancelled by scrolling: extraction is cheap, scoped to
    // the page and stays valid, so finishing it beats discarding it.
    bool fromCache = false;
    PageSize observedSize{};
    fz_display_list* list = acquireDisplayList(ctx, pageNumber, observedSize, fromCache);
    if (list == nullptr) {
        return;
    }

    std::shared_ptr<const std::string> runs = buildTextRuns(ctx, list, pageNumber);
    fz_drop_display_list(ctx, list);
    if (!runs) {
        return;
    }

    auto text = std::make_shared<PageText>();
    text->page = pageNumber;
    text->runs = std::move(runs);

    {
        std::lock_guard<std::mutex> guard(textMutex);
        textLayers.emplace(pageNumber, text);
    }
    // Recorded under wantedMutex only, never nested with textMutex, so the single
    // lock order (wantedMutex -> textMutex) is preserved.
    {
        std::lock_guard<std::mutex> guard(wantedMutex);
        textDelivered.insert(pageNumber);
    }
    textLayersBuilt.fetch_add(1);
    deliverText(std::move(text));
}

void PdfEngine::Impl::deliverText(std::shared_ptr<const PageText> text) {
    if (!open.load()) {
        return;
    }
    PageTextCallback callback;
    {
        std::lock_guard<std::mutex> guard(textCallbackMutex);
        callback = pageTextReady;
    }
    if (callback) {
        callback(std::move(text));
    }
}

fz_display_list* PdfEngine::Impl::acquireDisplayList(fz_context* ctx, int pageNumber, PageSize& size,
                                                     bool& fromCache) {
    {
        std::lock_guard<std::mutex> guard(displayListMutex);
        auto it = displayLists.find(pageNumber);
        if (it != displayLists.end()) {
            fz_keep_display_list(ctx, it->second.list);
            size = it->second.size;
            fromCache = true;
            displayListHits.fetch_add(1);
            return it->second.list;
        }
    }
    fromCache = false;

    // Only this block touches the document, so only this block needs the
    // document mutex. Nothing here may construct a C++ object with a
    // destructor: fz_throw longjmps and would skip it.
    fz_display_list* built = nullptr;
    fz_page* page = nullptr;
    PageSize bounds{};
    const bool documentAvailable = document != nullptr;
    if (documentAvailable) {
        std::lock_guard<std::mutex> guard(documentMutex);
        fz_var(page);
        fz_try(ctx) {
            page = fz_load_page(ctx, document, pageNumber);
            const fz_rect box = fz_bound_page(ctx, page);
            built = fz_new_display_list_from_page(ctx, page);
            bounds = PageSize{box.x1 - box.x0, box.y1 - box.y0};
        }
        fz_always(ctx) {
            fz_drop_page(ctx, page);
        }
        fz_catch(ctx) {
            built = nullptr;
        }
    }
    if (!built) {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(displayListMutex);
    auto [it, inserted] = displayLists.emplace(pageNumber, DisplayListEntry{built, bounds});
    if (!inserted) {
        fz_drop_display_list(ctx, built);
    }
    fz_keep_display_list(ctx, it->second.list);
    size = it->second.size;
    return it->second.list;
}

std::shared_ptr<const std::string> PdfEngine::Impl::rasteriseToBase64Png(fz_context* ctx,
                                                                        fz_display_list* list,
                                                                        float scale, int& outWidth,
                                                                        int& outHeight) {
    fz_pixmap* pixmap = nullptr;
    fz_buffer* buffer = nullptr;
    fz_output* output = nullptr;
    unsigned char* raw = nullptr;
    std::size_t rawLength = 0;
    bool encoded = false;

    fz_var(pixmap);
    fz_var(buffer);
    fz_var(output);

    const fz_matrix ctm = fz_scale(scale, scale);

    // No C++ objects with destructors inside fz_try: a longjmp would skip them.
    fz_try(ctx) {
        pixmap = fz_new_pixmap_from_display_list(ctx, list, ctm, fz_device_rgb(ctx), 0);
        outWidth = fz_pixmap_width(ctx, pixmap);
        outHeight = fz_pixmap_height(ctx, pixmap);

        buffer = fz_new_buffer(ctx, 64 * 1024);
        output = fz_new_output_with_buffer(ctx, buffer);
        fz_write_pixmap_as_png(ctx, output, pixmap);
        fz_close_output(ctx, output);

        rawLength = fz_buffer_storage(ctx, buffer, &raw);
        encoded = true;
    }
    fz_always(ctx) {
        fz_drop_output(ctx, output);
        fz_drop_pixmap(ctx, pixmap);
    }
    fz_catch(ctx) {
        encoded = false;
    }

    std::shared_ptr<const std::string> result;
    if (encoded && raw != nullptr) {
        result = std::make_shared<const std::string>(base64Encode(raw, rawLength));
    }

    fz_drop_buffer(ctx, buffer);
    return result;
}

void PdfEngine::Impl::deliver(std::shared_ptr<const RenderedPage> page) {
    if (!open.load()) {
        return;
    }
    PageReadyCallback callback;
    {
        std::lock_guard<std::mutex> guard(callbackMutex);
        callback = pageReady;
    }
    if (callback) {
        callback(std::move(page));
    }
}

void PdfEngine::Impl::runRenderJob(int pageNumber, PageSize pageSize, float scale,
                                   std::uint64_t key) {
    // Whatever happens, stop advertising this key as in flight.
    struct InflightRelease {
        Impl& impl;
        std::uint64_t key;
        ~InflightRelease() { impl.clearInflight(key); }
    } release{*this, key};

    const int workerIndex = ThreadPool::workerIndex();
    if (workerIndex < 0 || static_cast<std::size_t>(workerIndex) >= workerContexts.size()) {
        return;
    }
    fz_context* ctx = workerContexts[static_cast<std::size_t>(workerIndex)];
    if (ctx == nullptr || !open.load() || !isWanted(key)) {
        jobsCancelled.fetch_add(1);
        return;
    }

    // Never rasterise beyond the dimension cap.
    const float longestEdge = std::max(pageSize.width, pageSize.height);
    float effectiveScale = scale;
    if (longestEdge > 0.0f) {
        effectiveScale = std::min(scale, static_cast<float>(kMaxRenderDimension) / longestEdge);
    }

    bool fromCache = false;
    PageSize observedSize{};
    fz_display_list* list = acquireDisplayList(ctx, pageNumber, observedSize, fromCache);
    if (list == nullptr) {
        jobsCancelled.fetch_add(1);
        return;
    }

    // The viewport may have moved on while we waited for the document lock.
    if (!isWanted(key)) {
        fz_drop_display_list(ctx, list);
        jobsCancelled.fetch_add(1);
        return;
    }

    int pixelWidth = 0;
    int pixelHeight = 0;
    std::shared_ptr<const std::string> png =
        rasteriseToBase64Png(ctx, list, effectiveScale, pixelWidth, pixelHeight);
    fz_drop_display_list(ctx, list);

    if (!png || !isWanted(key)) {
        jobsCancelled.fetch_add(1);
        return;
    }

    auto rendered = std::make_shared<RenderedPage>();
    rendered->page = pageNumber;
    rendered->scale = effectiveScale;
    rendered->pixelWidth = pixelWidth;
    rendered->pixelHeight = pixelHeight;
    rendered->base64Png = std::move(png);

    pagesRasterised.fetch_add(1);
    storeBitmap(key, rendered);
    deliver(std::move(rendered));
}

// Replaces one run's glyphs (or inserts new text). For a replacement a text-only
// redaction removes the old characters (images and vector art are deliberately
// left untouched by the chosen options), then a text object draws the new text at
// the same baseline with the chosen font and colour.
void PdfEngine::Impl::applyEditToPage(fz_context* ctx, pdf_document* pdoc, pdf_page* page,
                                      pdf_obj* pageObj, const TextEdit& edit, float pageHeight) {
    const float size = edit.size > 0.0f ? edit.size : 12.0f;

    if (edit.replace) {
        fz_rect rect;
        const float pad = 1.5f;
        rect.x0 = edit.x - pad;
        rect.x1 = edit.x + std::max(edit.width, size) + pad;
        rect.y0 = edit.y - edit.ascender * size - pad;
        rect.y1 = edit.y + 0.30f * size + pad;

        pdf_annot* annot = pdf_create_annot(ctx, page, PDF_ANNOT_REDACT);
        pdf_set_annot_rect(ctx, annot, rect);
        pdf_redact_options redact{};
        redact.black_boxes = 0;
        redact.image_method = PDF_REDACT_IMAGE_NONE;
        redact.line_art = PDF_REDACT_LINE_ART_NONE;
        redact.text = PDF_REDACT_TEXT_REMOVE;
        pdf_apply_redaction(ctx, annot, &redact);  // also removes the annotation
    }

    char resourceName[96]{};
    bool cid = false;
    fz_font* embedFont = nullptr;
    resolveFontResource(ctx, pdoc, page, pageObj, edit.font, resourceName, sizeof(resourceName), cid,
                        &embedFont);

    fz_buffer* content = fz_new_buffer(ctx, 128);
    appendTextObject(ctx, content, resourceName, size, edit.angle, edit.x, pageHeight - edit.y,
                     edit.r, edit.g, edit.b, cid, edit.text, embedFont);
    pdf_obj* stream = pdf_add_stream(ctx, pdoc, content, nullptr, 0);
    fz_drop_buffer(ctx, content);
    attachContent(ctx, pdoc, page, pageObj, stream);
}

// Picks the font to draw with, adding it to the page's resources if it is not
// already there: an embedded face becomes a CID font (created once per document
// and shared), a standard face becomes a Type1 reference with WinAnsi encoding.
void PdfEngine::Impl::resolveFontResource(fz_context* ctx, pdf_document* pdoc, pdf_page* page,
                                          pdf_obj* pageObj, const std::string& fontId,
                                          char* resourceName, std::size_t nameSize, bool& cid,
                                          fz_font** embedFont) {
    cid = false;
    *embedFont = nullptr;

    pdf_obj* resources = pdf_page_resources(ctx, page);
    if (resources == nullptr) {
        pdf_dict_puts_drop(ctx, pageObj, "Resources", pdf_new_dict(ctx, pdoc, 1));
        resources = pdf_page_resources(ctx, page);
    }
    pdf_obj* fonts = pdf_dict_gets(ctx, resources, "Font");
    if (fonts == nullptr) {
        pdf_dict_puts_drop(ctx, resources, "Font", pdf_new_dict(ctx, pdoc, 1));
        fonts = pdf_dict_gets(ctx, resources, "Font");
    }

    const FontInfo* info = fontId.empty() ? nullptr : findFont(fontId);
    if (info != nullptr && info->embedded) {
        cid = true;
        *embedFont = acquireEmbeddedFont(ctx, *info);
        pdf_obj* fontRef = acquireCidFont(ctx, pdoc, *info);
        resourceNameFor(info->id, resourceName, nameSize);
        if (pdf_dict_gets(ctx, fonts, resourceName) == nullptr) {
            pdf_dict_puts(ctx, fonts, resourceName, fontRef);
        }
        return;
    }

    const char* base =
        (info != nullptr && !info->baseFont.empty()) ? info->baseFont.c_str() : "Helvetica";
    resourceNameFor(base, resourceName, nameSize);
    if (pdf_dict_gets(ctx, fonts, resourceName) == nullptr) {
        pdf_obj* fontDict = pdf_new_dict(ctx, pdoc, 6);
        pdf_dict_puts_drop(ctx, fontDict, "Type", pdf_new_name(ctx, "Font"));
        pdf_dict_puts_drop(ctx, fontDict, "Subtype", pdf_new_name(ctx, "Type1"));
        pdf_dict_puts_drop(ctx, fontDict, "BaseFont", pdf_new_name(ctx, base));
        pdf_dict_puts_drop(ctx, fontDict, "Encoding", pdf_new_name(ctx, "WinAnsiEncoding"));
        pdf_obj* fontRef = pdf_add_object_drop(ctx, pdoc, fontDict);
        pdf_dict_puts(ctx, fonts, resourceName, fontRef);
        pdf_drop_obj(ctx, fontRef);
    }
}

void PdfEngine::Impl::attachContent(fz_context* ctx, pdf_document* pdoc, pdf_page* page,
                                    pdf_obj* pageObj, pdf_obj* stream) {
    pdf_obj* contents = pdf_page_contents(ctx, page);
    if (pdf_is_array(ctx, contents)) {
        pdf_array_push(ctx, contents, stream);
    } else {
        pdf_obj* array = pdf_new_array(ctx, pdoc, 2);
        if (contents != nullptr) {
            pdf_array_push(ctx, array, contents);
        }
        pdf_array_push(ctx, array, stream);
        pdf_dict_puts_drop(ctx, pageObj, "Contents", array);
    }
    pdf_drop_obj(ctx, stream);
}

// Paragraph edit: one erase box over the whole selection, then each line is drawn
// on its original baseline so the leading and left margin are preserved. Extra
// lines continue below the last one at the same leading.
void PdfEngine::Impl::applyBlockToPage(fz_context* ctx, pdf_document* pdoc, pdf_page* page,
                                       pdf_obj* pageObj, const TextBlockEdit& edit,
                                       float pageHeight) {
    const float baseSize = edit.size > 0.0f ? edit.size : 12.0f;

    fz_rect rect;
    rect.x0 = std::min(edit.x0, edit.x1) - 1.5f;
    rect.x1 = std::max(edit.x0, edit.x1) + 1.5f;
    rect.y0 = std::min(edit.y0, edit.y1) - 1.5f;
    rect.y1 = std::max(edit.y0, edit.y1) + 1.5f;
    pdf_annot* annot = pdf_create_annot(ctx, page, PDF_ANNOT_REDACT);
    pdf_set_annot_rect(ctx, annot, rect);
    pdf_redact_options redact{};
    redact.black_boxes = 0;
    redact.image_method = PDF_REDACT_IMAGE_NONE;
    redact.line_art = PDF_REDACT_LINE_ART_NONE;
    redact.text = PDF_REDACT_TEXT_REMOVE;
    pdf_apply_redaction(ctx, annot, &redact);

    char resourceName[96]{};
    bool cid = false;
    fz_font* embedFont = nullptr;
    resolveFontResource(ctx, pdoc, page, pageObj, edit.font, resourceName, sizeof(resourceName), cid,
                        &embedFont);

    fz_buffer* content = fz_new_buffer(ctx, 256);

    float x = edit.lines.empty() ? std::min(edit.x0, edit.x1) : edit.lines[0].x;
    const float firstBaseline =
        edit.lines.empty() ? (std::min(edit.y0, edit.y1) + baseSize) : edit.lines[0].y;
    float baseline = firstBaseline;
    float lastSize = baseSize;
    const float spacing = edit.spacing > 0.05f ? edit.spacing : 1.0f;
    std::size_t index = 0;

    const std::string_view text(edit.text);
    std::size_t start = 0;
    for (;;) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;

        float lineX = x;
        float lineY = baseline;
        float lineSize = lastSize;
        if (index < edit.lines.size()) {
            lineX = edit.lines[index].x;
            // Scale the gap between baselines around the first line, so spacing
            // can be widened or tightened without moving the block's top.
            lineY = firstBaseline + (edit.lines[index].y - firstBaseline) * spacing;
            lineSize = edit.lines[index].size > 0.0f ? edit.lines[index].size : baseSize;
        } else if (index > 0) {
            lineY = baseline + lastSize * 1.2f * spacing;
        }

        appendTextObject(ctx, content, resourceName, lineSize, 0.0f, lineX, pageHeight - lineY,
                         edit.r, edit.g, edit.b, cid, text.substr(start, end - start), embedFont);

        x = lineX;
        baseline = lineY;
        lastSize = lineSize;
        ++index;

        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }

    pdf_obj* stream = pdf_add_stream(ctx, pdoc, content, nullptr, 0);
    fz_drop_buffer(ctx, content);
    attachContent(ctx, pdoc, page, pageObj, stream);
}

// ------------------------------------------------------------------ public --

PdfEngine::PdfEngine() : impl_(std::make_unique<Impl>()) {
    impl_->locksContext.user = &impl_->locks;
    impl_->locksContext.lock = lockHook;
    impl_->locksContext.unlock = unlockHook;

    impl_->baseContext = fz_new_context(nullptr, &impl_->locksContext, FZ_STORE_DEFAULT);
    if (impl_->baseContext == nullptr) {
        throw std::runtime_error("fz_new_context failed - out of memory");
    }
    fz_register_document_handlers(impl_->baseContext);

    const unsigned int hardware = std::thread::hardware_concurrency();
    std::size_t workers = std::clamp<std::size_t>(hardware == 0 ? 4u : hardware, 2, 12);
    if (workers > 4) {
        --workers;  // keep a core for the UI thread and WebView2
    }

    impl_->workerContexts.assign(workers, nullptr);
    impl_->pool = std::make_unique<ThreadPool>(
        workers, "rpfg-render",
        [this](int index) {
            // Each worker gets its own context, cloned from the parent so that
            // they all share one locked resource store.
            impl_->workerContexts[static_cast<std::size_t>(index)] =
                fz_clone_context(impl_->baseContext);
        },
        [this](int index) {
            fz_context*& ctx = impl_->workerContexts[static_cast<std::size_t>(index)];
            if (ctx != nullptr) {
                fz_drop_context(ctx);
                ctx = nullptr;
            }
        });

    logInfo("render pool started with " + std::to_string(workers) + " workers");
}

PdfEngine::~PdfEngine() {
    if (!impl_) {
        return;
    }
    impl_->shuttingDown.store(true);
    impl_->open.store(false);
    {
        std::lock_guard<std::mutex> guard(impl_->wantedMutex);
        impl_->wanted.clear();
    }

    // Stop the pool first: workers drop their cloned contexts, and only then
    // is it safe to drop the parent context.
    impl_->pool.reset();
    impl_->clearDisplayLists();
    impl_->clearBitmaps();
    impl_->clearText();
    impl_->dropFonts(impl_->baseContext);
    if (impl_->document != nullptr) {
        fz_drop_document(impl_->baseContext, impl_->document);
        impl_->document = nullptr;
    }
    fz_drop_context(impl_->baseContext);
}

void PdfEngine::setPageReadyCallback(PageReadyCallback callback) {
    std::lock_guard<std::mutex> guard(impl_->callbackMutex);
    impl_->pageReady = std::move(callback);
}

void PdfEngine::setPageTextCallback(PageTextCallback callback) {
    std::lock_guard<std::mutex> guard(impl_->textCallbackMutex);
    impl_->pageTextReady = std::move(callback);
}

std::optional<DocumentInfo> PdfEngine::openDocument(const std::string& utf8Path, std::string& error) {
    closeDocument();

    Impl& state = *impl_;
    fz_context* ctx = state.baseContext;

    fz_document* document = nullptr;
    fz_page* page = nullptr;
    int pageCount = 0;
    char titleBuffer[512]{};
    std::string failure;

    fz_var(document);
    fz_var(page);

    {
        // The lock is taken outside fz_try, and nothing inside allocates C++
        // memory: fz_throw longjmps, which would skip destructors and unwind
        // straight through MuPDF's try stack.
        std::lock_guard<std::mutex> guard(state.documentMutex);
        fz_try(ctx) {
            document = fz_open_document(ctx, utf8Path.c_str());
            if (fz_needs_password(ctx, document)) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "document is password protected");
            }
            pageCount = fz_count_pages(ctx, document);
            fz_lookup_metadata(ctx, document, FZ_META_INFO_TITLE, titleBuffer, sizeof(titleBuffer));
        }
        fz_catch(ctx) {
            failure = fz_caught_message(ctx);
            fz_drop_document(ctx, document);
            document = nullptr;
        }
    }

    if (document == nullptr) {
        error = failure.empty() ? "could not open document" : failure;
        logError("open failed for '" + utf8Path + "': " + error);
        return std::nullopt;
    }

    // Size the geometry vector here, outside fz_try, so a std::bad_alloc
    // cannot escape through MuPDF's longjmp-based try stack.
    DocumentInfo info;
    info.path = utf8Path;
    info.title = titleBuffer;
    info.pageCount = pageCount;
    info.pages.assign(static_cast<std::size_t>(std::max(0, pageCount)), PageSize{});

    bool geometryOk = true;
    {
        std::lock_guard<std::mutex> guard(state.documentMutex);
        fz_try(ctx) {
            for (int i = 0; i < pageCount; ++i) {
                page = fz_load_page(ctx, document, i);
                const fz_rect box = fz_bound_page(ctx, page);
                fz_drop_page(ctx, page);
                page = nullptr;
                info.pages[static_cast<std::size_t>(i)] =
                    PageSize{box.x1 - box.x0, box.y1 - box.y0};
            }
        }
        fz_always(ctx) {
            fz_drop_page(ctx, page);
        }
        fz_catch(ctx) {
            failure = fz_caught_message(ctx);
            geometryOk = false;
        }
    }

    if (!geometryOk) {
        fz_try(ctx) {
            fz_drop_document(ctx, document);
        }
        fz_catch(ctx) {
            // The document is being discarded either way.
        }
        error = failure.empty() ? "could not read page geometry" : failure;
        logError("open failed for '" + utf8Path + "': " + error);
        return std::nullopt;
    }

    {
        std::lock_guard<std::mutex> guard(state.documentMutex);
        state.document = document;
        state.info = std::move(info);
        state.open.store(true);
        state.fullSaveDone = false;
        state.dirty.store(false);
    }
    {
        std::lock_guard<std::mutex> guard(state.wantedMutex);
        state.wanted.clear();
    }

    logInfo("opened '" + state.info.path + "' with " + std::to_string(state.info.pageCount) +
            " pages");
    return state.info;
}

void PdfEngine::closeDocument() {
    Impl& state = *impl_;
    state.open.store(false);
    state.fullSaveDone = false;
    state.dirty.store(false);
    {
        // Dropping the wanted set cancels anything queued.
        std::lock_guard<std::mutex> guard(state.wantedMutex);
        state.wanted.clear();
        state.textDelivered.clear();
    }

    // Embedded CID font objects belong to this document, so drop them first.
    state.dropFonts(state.baseContext);

    {
        std::lock_guard<std::mutex> guard(state.documentMutex);
        if (state.document != nullptr) {
            fz_try(state.baseContext) {
                fz_drop_document(state.baseContext, state.document);
            }
            fz_catch(state.baseContext) {
                // Nothing useful to do; the document is going away regardless.
            }
            state.document = nullptr;
        }
        state.info = DocumentInfo{};
    }

    state.clearDisplayLists();
    state.clearBitmaps();
    state.clearText();
    {
        std::lock_guard<std::mutex> guard(state.inflightMutex);
        state.inflight.clear();
    }
}

bool PdfEngine::isOpen() const {
    return impl_->open.load();
}

bool PdfEngine::applyTextEdit(const TextEdit& edit, std::string& error) {
    Impl& state = *impl_;
    if (!state.open.load()) {
        error = "no document is open";
        return false;
    }
    if (edit.page < 0 || edit.page >= state.info.pageCount) {
        error = "page is out of range";
        return false;
    }

    std::lock_guard<std::mutex> guard(state.documentMutex);
    fz_context* ctx = state.baseContext;
    pdf_document* pdoc = pdf_specifics(ctx, state.document);
    if (pdoc == nullptr) {
        error = "editing is only supported for PDF documents";
        return false;
    }

    pdf_page* page = nullptr;
    bool ok = false;
    std::string failure;
    fz_var(page);
    fz_try(ctx) {
        page = pdf_load_page(ctx, pdoc, edit.page);
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdoc, edit.page);
        const float pageHeight = state.info.pages[static_cast<std::size_t>(edit.page)].height;
        state.applyEditToPage(ctx, pdoc, page, pageObj, edit, pageHeight);
        ok = true;
    }
    fz_always(ctx) {
        pdf_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        failure = fz_caught_message(ctx);
        ok = false;
    }

    if (!ok) {
        error = failure.empty() ? "the edit could not be applied" : failure;
        logError("text edit on page " + std::to_string(edit.page + 1) + " failed: " + error);
        return false;
    }

    state.invalidatePage(edit.page);
    state.editsApplied.fetch_add(1);
    state.dirty.store(true);
    logInfo("edited page " + std::to_string(edit.page + 1) + " (" +
            std::to_string(edit.text.size()) + " chars)");
    return true;
}

bool PdfEngine::applyTextBlock(const TextBlockEdit& edit, std::string& error) {
    Impl& state = *impl_;
    if (!state.open.load()) {
        error = "no document is open";
        return false;
    }
    if (edit.page < 0 || edit.page >= state.info.pageCount) {
        error = "page is out of range";
        return false;
    }

    std::lock_guard<std::mutex> guard(state.documentMutex);
    fz_context* ctx = state.baseContext;
    pdf_document* pdoc = pdf_specifics(ctx, state.document);
    if (pdoc == nullptr) {
        error = "editing is only supported for PDF documents";
        return false;
    }

    pdf_page* page = nullptr;
    bool ok = false;
    std::string failure;
    fz_var(page);
    fz_try(ctx) {
        page = pdf_load_page(ctx, pdoc, edit.page);
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdoc, edit.page);
        const float pageHeight = state.info.pages[static_cast<std::size_t>(edit.page)].height;
        state.applyBlockToPage(ctx, pdoc, page, pageObj, edit, pageHeight);
        ok = true;
    }
    fz_always(ctx) {
        pdf_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        failure = fz_caught_message(ctx);
        ok = false;
    }

    if (!ok) {
        error = failure.empty() ? "the edit could not be applied" : failure;
        logError("block edit on page " + std::to_string(edit.page + 1) + " failed: " + error);
        return false;
    }

    state.invalidatePage(edit.page);
    state.editsApplied.fetch_add(1);
    state.dirty.store(true);
    logInfo("edited a block on page " + std::to_string(edit.page + 1) + " (" +
            std::to_string(edit.lines.size()) + " lines)");
    return true;
}

bool PdfEngine::eraseRegion(int page, float x0, float y0, float x1, float y1, std::string& error) {
    Impl& state = *impl_;
    if (!state.open.load()) {
        error = "no document is open";
        return false;
    }
    if (page < 0 || page >= state.info.pageCount) {
        error = "page is out of range";
        return false;
    }

    std::lock_guard<std::mutex> guard(state.documentMutex);
    fz_context* ctx = state.baseContext;
    pdf_document* pdoc = pdf_specifics(ctx, state.document);
    if (pdoc == nullptr) {
        error = "editing is only supported for PDF documents";
        return false;
    }

    fz_rect rect{};
    rect.x0 = std::min(x0, x1);
    rect.x1 = std::max(x0, x1);
    rect.y0 = std::min(y0, y1);
    rect.y1 = std::max(y0, y1);

    pdf_page* p = nullptr;
    bool ok = false;
    std::string failure;
    fz_var(p);
    fz_try(ctx) {
        p = pdf_load_page(ctx, pdoc, page);
        pdf_annot* annot = pdf_create_annot(ctx, p, PDF_ANNOT_REDACT);
        pdf_set_annot_rect(ctx, annot, rect);
        // A redaction with images and line art switched on deletes everything the
        // box covers - shapes, table rules, images and text - rather than hiding
        // it. black_boxes is off so nothing is painted in its place.
        pdf_redact_options opts{};
        opts.black_boxes = 0;
        opts.image_method = PDF_REDACT_IMAGE_REMOVE;
        opts.line_art = PDF_REDACT_LINE_ART_REMOVE_IF_COVERED;
        opts.text = PDF_REDACT_TEXT_REMOVE;
        pdf_apply_redaction(ctx, annot, &opts);
        ok = true;
    }
    fz_always(ctx) {
        pdf_drop_page(ctx, p);
    }
    fz_catch(ctx) {
        failure = fz_caught_message(ctx);
        ok = false;
    }
    if (!ok) {
        error = failure.empty() ? "the erase could not be applied" : failure;
        logError("erase on page " + std::to_string(page + 1) + " failed: " + error);
        return false;
    }

    state.invalidatePage(page);
    state.editsApplied.fetch_add(1);
    state.dirty.store(true);
    logInfo("erased a region on page " + std::to_string(page + 1));
    return true;
}

std::vector<FontInfo> PdfEngine::availableFonts() const {
    impl_->buildFontList();
    std::lock_guard<std::mutex> guard(impl_->fontsMutex);
    return impl_->fonts;
}

std::shared_ptr<const PageModel> PdfEngine::pageModel(int page) const {
    std::lock_guard<std::mutex> guard(impl_->textMutex);
    auto it = impl_->textModels.find(page);
    return it == impl_->textModels.end() ? nullptr : it->second;
}

void PdfEngine::setFontsDirectory(const std::string& utf8Dir) {
    {
        std::lock_guard<std::mutex> guard(impl_->fontsMutex);
        impl_->fontsDir = utf8Dir;
        impl_->fontsBuilt = false;
        impl_->fonts.clear();
    }
    impl_->buildFontList();
}

bool PdfEngine::saveDocument(const std::string& utf8Path, SaveMode mode, std::string& error) {
    Impl& state = *impl_;
    if (!state.open.load()) {
        error = "no document is open";
        return false;
    }
    if (utf8Path.empty()) {
        error = "no path to save to";
        return false;
    }

    std::lock_guard<std::mutex> guard(state.documentMutex);
    fz_context* ctx = state.baseContext;
    pdf_document* pdoc = pdf_specifics(ctx, state.document);
    if (pdoc == nullptr) {
        error = "saving is only supported for PDF documents";
        return false;
    }

    auto write = [&](const std::string& target, bool incremental) -> bool {
        pdf_write_options opts;
        pdf_init_write_options(ctx, &opts);
        opts.do_incremental = incremental ? 1 : 0;
        if (!incremental) {
            opts.do_garbage = 1;
            opts.do_compress = 1;
            opts.do_compress_fonts = 1;
        }
        bool written = false;
        fz_try(ctx) {
            pdf_save_document(ctx, pdoc, target.c_str(), &opts);
            written = true;
        }
        fz_catch(ctx) {
            error = fz_caught_message(ctx);
            written = false;
        }
        return written;
    };

    const bool samePath = utf8Path == state.info.path;
    // A full write to the file we are reading from goes via a temp file and an
    // atomic replace - pdf_save_document cannot overwrite its own input safely.
    auto writeReplacing = [&](const std::string& target) -> bool {
        const std::string temp = target + ".rpfg-tmp";
        if (!write(temp, false)) {
            return false;
        }
        if (MoveFileExW(utf8ToWide(temp).c_str(), utf8ToWide(target).c_str(),
                        MOVEFILE_REPLACE_EXISTING) == 0) {
            error = "could not replace " + target;
            DeleteFileW(utf8ToWide(temp).c_str());
            return false;
        }
        return true;
    };

    bool ok = false;
    bool usedIncremental = false;
    if (mode == SaveMode::Incremental && !state.fullSaveDone && samePath) {
        ok = write(utf8Path, true);
        usedIncremental = ok;
        if (!ok) {
            // Repaired files, or a document opened read-only, cannot be appended
            // to; fall back to a full rewrite of the same path.
            error.clear();
            logInfo("incremental save unavailable, rewriting the file in full");
            ok = writeReplacing(utf8Path);
        }
    } else if (samePath) {
        ok = writeReplacing(utf8Path);
    } else {
        ok = write(utf8Path, false);
    }

    if (!ok) {
        logError("save to '" + utf8Path + "' failed: " + error);
        return false;
    }

    if (usedIncremental) {
        state.dirty.store(false);
    } else if (mode == SaveMode::Copy) {
        // The document still belongs to its original path; it stays dirty.
    } else {
        state.info.path = utf8Path;
        state.fullSaveDone = true;
        state.dirty.store(false);
    }

    logInfo(std::string(usedIncremental ? "incrementally saved '" : "saved '") + utf8Path + "'");
    return true;
}

bool PdfEngine::hasUnsavedChanges() const {
    return impl_->dirty.load();
}

void PdfEngine::markSaved() {
    impl_->dirty.store(false);
}

std::string PdfEngine::currentPath() const {
    return impl_->info.path;
}

void PdfEngine::requestViewport(const std::vector<int>& pages, float scale) {
    Impl& state = *impl_;
    if (!state.open.load() || state.info.pageCount == 0) {
        return;
    }

    const int steps = scaleSteps(scale);
    const float quantisedScale = scaleFromSteps(steps);

    std::unordered_set<std::uint64_t> wantedKeys;
    std::unordered_set<int> wantedPages;
    std::vector<std::pair<int, PageSize>> rasterWork;
    std::vector<int> textWork;

    for (int pageNumber : pages) {
        if (pageNumber < 0 || pageNumber >= state.info.pageCount) {
            continue;
        }
        wantedPages.insert(pageNumber);

        const std::uint64_t key = bitmapKey(pageNumber, steps);
        wantedKeys.insert(key);

        // Text is page-scoped and never re-extracted once cached.
        if (state.lookupText(pageNumber) == nullptr && state.markTextInflight(pageNumber)) {
            textWork.push_back(pageNumber);
        }

        if (auto cached = state.lookupBitmap(key)) {
            state.deliver(cached);
            continue;
        }
        if (!state.markInflight(key)) {
            state.jobsCoalesced.fetch_add(1);  // an identical render is already queued
            continue;
        }

        state.jobsSubmitted.fetch_add(1);
        rasterWork.emplace_back(pageNumber, state.info.pages[static_cast<std::size_t>(pageNumber)]);
    }

    // Text the UI has since dropped (it releases layers for far-away pages) is
    // handed straight back when a page returns into view.
    std::vector<std::shared_ptr<const PageText>> resend;
    {
        std::lock_guard<std::mutex> guard(state.wantedMutex);
        state.wanted = std::move(wantedKeys);

        for (auto it = state.textDelivered.begin(); it != state.textDelivered.end();) {
            if (wantedPages.count(*it) != 0) {
                ++it;
            } else {
                it = state.textDelivered.erase(it);
            }
        }
        for (int pageNumber : wantedPages) {
            if (state.textDelivered.count(pageNumber) != 0) {
                continue;
            }
            if (auto cached = state.lookupText(pageNumber)) {
                state.textDelivered.insert(pageNumber);
                resend.push_back(std::move(cached));
            }
        }
    }
    for (const std::shared_ptr<const PageText>& text : resend) {
        state.deliverText(text);
    }

    // Rasters first; text extraction shares the pool but must never delay the
    // pixels the user is actually looking at.
    for (std::size_t i = 0; i < rasterWork.size(); ++i) {
        const int pageNumber = rasterWork[i].first;
        const PageSize size = rasterWork[i].second;
        const std::uint64_t key = bitmapKey(pageNumber, steps);
        state.pool->submit(static_cast<int>(i), [&state, pageNumber, size, quantisedScale, key]() {
            state.runRenderJob(pageNumber, size, quantisedScale, key);
        });
    }

    const int textPriority = static_cast<int>(rasterWork.size()) + 1000;
    for (std::size_t i = 0; i < textWork.size(); ++i) {
        const int pageNumber = textWork[i];
        state.pool->submit(textPriority + static_cast<int>(i), [&state, pageNumber]() {
            state.runTextJob(pageNumber);
        });
    }
}

EngineStats PdfEngine::stats() const {
    const Impl& state = *impl_;
    EngineStats stats;
    stats.workerThreads = static_cast<int>(state.pool ? state.pool->threadCount() : 0);
    stats.queuedJobs = static_cast<int>(state.pool ? state.pool->queued() : 0);
    stats.jobsSubmitted = state.jobsSubmitted.load();
    stats.jobsCoalesced = state.jobsCoalesced.load();
    stats.jobsCancelled = state.jobsCancelled.load();
    stats.pagesRasterised = state.pagesRasterised.load();
    stats.displayListHits = state.displayListHits.load();
    stats.bitmapCacheHits = state.bitmapHits.load();
    {
        std::lock_guard<std::mutex> guard(state.bitmapMutex);
        stats.bitmapCacheBytes = state.bitmapBytes;
        stats.bitmapCacheEntries = state.bitmaps.size();
    }
    stats.textLayersBuilt = state.textLayersBuilt.load();
    stats.editsApplied = state.editsApplied.load();
    {
        std::lock_guard<std::mutex> guard(state.textMutex);
        stats.textCacheEntries = state.textLayers.size();
    }
    return stats;
}

}  // namespace rpfg
