#include "render/DocumentRenderer.h"

#include <mupdf/fitz.h>
#include <cmath>
#include <map>

#include "document/Document.h"
#include "document/ImageObject.h"
#include "document/LineObject.h"
#include "document/ProtectedObject.h"
#include "document/ShapeObject.h"
#include "document/TextFrame.h"
#include "document/VectorObject.h"
#include "util/Base64.h"

namespace rpfg {
namespace {

std::string mapToBase14(const std::string& face, bool bold, bool italic) {
    std::string lower;
    for (char c : face) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    const bool isMono = lower.find("courier") != std::string::npos || lower.find("mono") != std::string::npos;
    const bool isSerif = lower.find("times") != std::string::npos || lower.find("serif") != std::string::npos || lower.find("roman") != std::string::npos;

    if (isMono) {
        if (bold && italic) return "Courier-BoldOblique";
        if (bold) return "Courier-Bold";
        if (italic) return "Courier-Oblique";
        return "Courier";
    }
    if (isSerif) {
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

DocumentRenderer::DocumentRenderer() = default;
DocumentRenderer::~DocumentRenderer() = default;

void renderPageToDevice(fz_context* ctx, fz_device* dev, const Page& page, fz_matrix pageCtm,
                        const Document* doc, std::map<std::string, fz_font*>& fontCache) {
    fz_colorspace* rgbCs = fz_device_rgb(ctx);

    auto getFont = [&](const TextStyle& style) -> fz_font* {
        // 1. Check embedded fonts from document
        if (doc != nullptr && !style.fontId.empty()) {
            const DocumentFont* docFont = doc->findFont(style.fontId);
            if (docFont != nullptr && docFont->isEmbedded && !docFont->fontData.empty()) {
                auto it = fontCache.find(style.fontId);
                if (it != fontCache.end()) return it->second;
                fz_font* f = nullptr;
                fz_try(ctx) {
                    f = fz_new_font_from_memory(ctx, docFont->postscriptName.c_str(),
                                                docFont->fontData.data(),
                                                static_cast<int>(docFont->fontData.size()),
                                                0, 1);
                }
                fz_catch(ctx) {
                    f = nullptr;
                }
                if (f != nullptr) {
                    fontCache[style.fontId] = f;
                    return f;
                }
            }
        }

        // 2. Base-14 fallback
        const std::string baseName = mapToBase14(style.fontName, style.bold, style.italic);
        auto it = fontCache.find(baseName);
        if (it != fontCache.end()) return it->second;
        fz_font* f = fz_new_base14_font(ctx, baseName.c_str());
        fontCache[baseName] = f;
        return f;
    };

    // Draw document objects ordered by zIndex
    for (const auto& obj : page.objects()) {
        if (!obj || !obj->isVisible()) continue;

        const bool hasClip = obj->clipRect().has_value() || obj->hasClipPath();
        if (hasClip) {
            fz_path* cp = fz_new_path(ctx);
            fz_rect fzCr;
            if (obj->hasClipPath()) {
                for (const auto& cmd : obj->clipPath()) {
                    switch (cmd.verb) {
                        case PathVerb::MoveTo:
                            fz_moveto(ctx, cp, cmd.p0.x, cmd.p0.y);
                            break;
                        case PathVerb::LineTo:
                            fz_lineto(ctx, cp, cmd.p0.x, cmd.p0.y);
                            break;
                        case PathVerb::CubicTo:
                            fz_curveto(ctx, cp, cmd.p0.x, cmd.p0.y, cmd.p1.x, cmd.p1.y, cmd.p2.x, cmd.p2.y);
                            break;
                        case PathVerb::Close:
                            fz_closepath(ctx, cp);
                            break;
                    }
                }
                fzCr = fz_bound_path(ctx, cp, nullptr, fz_identity);
                if (obj->clipRect().has_value()) {
                    const Rect& cr = *obj->clipRect();
                    fz_rect scissorRect = fz_make_rect(cr.x0, cr.y0, cr.x1, cr.y1);
                    fzCr = fz_intersect_rect(fzCr, scissorRect);
                }
            } else {
                const Rect& cr = *obj->clipRect();
                fz_rectto(ctx, cp, cr.x0, cr.y0, cr.x1, cr.y1);
                fzCr = fz_make_rect(cr.x0, cr.y0, cr.x1, cr.y1);
            }
            fz_clip_path(ctx, dev, cp, 0, pageCtm, fz_transform_rect(fzCr, pageCtm));
            fz_drop_path(ctx, cp);
        }

        if (obj->type() == ObjectType::Line) {
            auto line = std::static_pointer_cast<LineObject>(obj);
            fz_path* path = fz_new_path(ctx);
            fz_moveto(ctx, path, line->startPoint().x, line->startPoint().y);
            fz_lineto(ctx, path, line->endPoint().x, line->endPoint().y);
            fz_stroke_state stroke{};
            stroke.linewidth = line->strokeWidth();
            const float col[3] = {line->color().r, line->color().g, line->color().b};
            fz_stroke_path(ctx, dev, path, &stroke, pageCtm, rgbCs, col, line->color().a, fz_default_color_params);
            fz_drop_path(ctx, path);
        }
        else if (obj->type() == ObjectType::Shape) {
            auto shape = std::static_pointer_cast<ShapeObject>(obj);
            fz_rect r = fz_make_rect(shape->bounds().x0, shape->bounds().y0, shape->bounds().x1, shape->bounds().y1);
            fz_path* path = fz_new_path(ctx);
            fz_rectto(ctx, path, r.x0, r.y0, r.x1, r.y1);
            if (shape->isFilled()) {
                const float fcol[3] = {shape->fillColor().r, shape->fillColor().g, shape->fillColor().b};
                fz_fill_path(ctx, dev, path, 0, pageCtm, rgbCs, fcol, shape->fillColor().a, fz_default_color_params);
            }
            if (shape->isStroked()) {
                fz_stroke_state stroke{};
                stroke.linewidth = shape->strokeWidth();
                const float scol[3] = {shape->strokeColor().r, shape->strokeColor().g, shape->strokeColor().b};
                fz_stroke_path(ctx, dev, path, &stroke, pageCtm, rgbCs, scol, shape->strokeColor().a, fz_default_color_params);
            }
            fz_drop_path(ctx, path);
        }
        else if (obj->type() == ObjectType::Vector) {
            auto vec = std::static_pointer_cast<VectorObject>(obj);
            if (!vec->commands().empty()) {
                fz_path* path = fz_new_path(ctx);
                for (const auto& cmd : vec->commands()) {
                    switch (cmd.verb) {
                        case PathVerb::MoveTo:
                            fz_moveto(ctx, path, cmd.p0.x, cmd.p0.y);
                            break;
                        case PathVerb::LineTo:
                            fz_lineto(ctx, path, cmd.p0.x, cmd.p0.y);
                            break;
                        case PathVerb::CubicTo:
                            fz_curveto(ctx, path, cmd.p0.x, cmd.p0.y, cmd.p1.x, cmd.p1.y, cmd.p2.x, cmd.p2.y);
                            break;
                        case PathVerb::Close:
                            fz_closepath(ctx, path);
                            break;
                    }
                }
                if (vec->isFilled()) {
                    const float fcol[3] = {vec->fillColor().r, vec->fillColor().g, vec->fillColor().b};
                    fz_fill_path(ctx, dev, path, vec->evenOdd() ? 1 : 0, pageCtm, rgbCs, fcol, vec->fillColor().a, fz_default_color_params);
                }
                if (vec->isStroked()) {
                    fz_stroke_state stroke{};
                    stroke.linewidth = vec->strokeWidth();
                    const float scol[3] = {vec->strokeColor().r, vec->strokeColor().g, vec->strokeColor().b};
                    fz_stroke_path(ctx, dev, path, &stroke, pageCtm, rgbCs, scol, 1.0f, fz_default_color_params);
                }
                fz_drop_path(ctx, path);
            }
        }
        else if (obj->type() == ObjectType::Image) {
            auto imgObj = std::static_pointer_cast<ImageObject>(obj);
            if (imgObj->pixelWidth() > 0 && imgObj->pixelHeight() > 0 && !imgObj->data().empty()) {
                fz_pixmap* imgPix = fz_new_pixmap(ctx, rgbCs, imgObj->pixelWidth(), imgObj->pixelHeight(), nullptr, 1);
                const int w = imgObj->pixelWidth();
                const int h = imgObj->pixelHeight();
                const unsigned char* src = imgObj->data().data();
                unsigned char* dst = fz_pixmap_samples(ctx, imgPix);
                const std::size_t numPixels = static_cast<std::size_t>(w) * h;
                const std::size_t count = std::min(imgObj->data().size(), numPixels * 4);
                for (std::size_t i = 0; i * 4 < count; ++i) {
                    const uint8_t r_c = src[i * 4 + 0];
                    const uint8_t g_c = src[i * 4 + 1];
                    const uint8_t b_c = src[i * 4 + 2];
                    const uint8_t a_c = src[i * 4 + 3];
                    dst[i * 4 + 0] = static_cast<uint8_t>((static_cast<uint32_t>(r_c) * a_c + 127) / 255);
                    dst[i * 4 + 1] = static_cast<uint8_t>((static_cast<uint32_t>(g_c) * a_c + 127) / 255);
                    dst[i * 4 + 2] = static_cast<uint8_t>((static_cast<uint32_t>(b_c) * a_c + 127) / 255);
                    dst[i * 4 + 3] = a_c;
                }

                fz_image* fzImg = fz_new_image_from_pixmap(ctx, imgPix, nullptr);
                fz_drop_pixmap(ctx, imgPix);

                fz_matrix imgCtm;
                const Matrix& t = imgObj->transform();
                if (!t.isIdentity()) {
                    imgCtm = fz_concat(fz_make_matrix(t.a, t.b, t.c, t.d, t.e, t.f), pageCtm);
                } else {
                    const Rect b = imgObj->bounds();
                    imgCtm = fz_concat(fz_make_matrix(b.width(), 0.0f, 0.0f, b.height(), b.x0, b.y0), pageCtm);
                }
                fz_fill_image(ctx, dev, fzImg, imgCtm, 1.0f, fz_default_color_params);
                fz_drop_image(ctx, fzImg);
            }
        }
        else if (obj->type() == ObjectType::TextFrame) {
            auto frame = std::static_pointer_cast<TextFrame>(obj);
            for (const auto& para : frame->paragraphs()) {
                for (const auto& run : para.runs()) {
                    if (run.empty()) continue;

                    const TextStyle& style = run.style();
                    fz_font* font = getFont(style);
                    if (font == nullptr) continue;

                    const float size = style.fontSize > 0.0f ? style.fontSize : 12.0f;
                    float baselineX = run.layoutBaseline().x;
                    float baselineY = run.layoutBaseline().y;

                    if (run.originalGeometry().hasOriginalGeometry) {
                        baselineX = run.originalGeometry().originalBaseline.x;
                        baselineY = run.originalGeometry().originalBaseline.y;
                    }

                    const fz_matrix trm = fz_make_matrix(size, 0.0f, 0.0f, -size, baselineX, baselineY);
                    fz_text* text = fz_new_text(ctx);
                    fz_show_string(ctx, text, font, trm, run.textUtf8().c_str(), 0, 0, FZ_BIDI_UNSET, static_cast<fz_text_language>(0));

                    const float col[3] = {style.color.r, style.color.g, style.color.b};
                    fz_fill_text(ctx, dev, text, pageCtm, rgbCs, col, style.color.a, fz_default_color_params);
                    fz_drop_text(ctx, text);
                }
            }
        }

        if (hasClip) {
            fz_pop_clip(ctx, dev);
        }
    }
}

static bool renderPageToPixmap(fz_context* ctx, const Page& page, const RenderOptions& options,
                               fz_pixmap* target, std::string& error, const Document* doc = nullptr) {
    const float scale = options.scale > 0.05f ? options.scale : 1.0f;
    const fz_matrix pageCtm = fz_scale(scale, scale);

    fz_device* dev = fz_new_draw_device(ctx, fz_identity, target);
    if (dev == nullptr) {
        error = "could not create draw device";
        return false;
    }

    fz_colorspace* rgbCs = fz_device_rgb(ctx);
    std::map<std::string, fz_font*> fontCache;

    fz_try(ctx) {
        renderPageToDevice(ctx, dev, page, pageCtm, doc, fontCache);

        // Draw Selection Rectangles
        for (const auto& selRect : options.selectionRectangles) {
            fz_path* p = fz_new_path(ctx);
            fz_rectto(ctx, p, selRect.x0, selRect.y0, selRect.x1, selRect.y1);
            const float selCol[3] = {0.2f, 0.5f, 0.9f};
            fz_fill_path(ctx, dev, p, 0, pageCtm, rgbCs, selCol, 0.35f, fz_default_color_params);
            fz_drop_path(ctx, p);
        }

        // Draw Caret
        if (options.showCaret) {
            fz_path* p = fz_new_path(ctx);
            const float cx = options.caretPosition.x;
            const float cy = options.caretPosition.y;
            fz_moveto(ctx, p, cx, cy - options.caretHeight * 0.8f);
            fz_lineto(ctx, p, cx, cy + options.caretHeight * 0.2f);
            fz_stroke_state stroke{};
            stroke.linewidth = 1.5f;
            const float caretCol[3] = {0.0f, 0.0f, 0.0f};
            fz_stroke_path(ctx, dev, p, &stroke, pageCtm, rgbCs, caretCol, 1.0f, fz_default_color_params);
            fz_drop_path(ctx, p);
        }

        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
        for (auto& entry : fontCache) {
            fz_drop_font(ctx, entry.second);
        }
    }
    fz_catch(ctx) {
        error = fz_caught_message(ctx);
        return false;
    }

    return true;
}

bool DocumentRenderer::renderPageGray(const Page& page, float scale, GrayImage& out, std::string& error, const Document* doc) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) {
        error = "could not create MuPDF context";
        return false;
    }
    fz_register_document_handlers(ctx);

    const int width = static_cast<int>(std::ceil(page.width() * scale));
    const int height = static_cast<int>(std::ceil(page.height() * scale));

    RenderOptions opts;
    opts.scale = scale;

    fz_pixmap* rgbPix = fz_new_pixmap(ctx, fz_device_rgb(ctx), width, height, nullptr, 0);
    fz_clear_pixmap_with_value(ctx, rgbPix, 255);

    bool ok = renderPageToPixmap(ctx, page, opts, rgbPix, error, doc);
    if (ok) {
        fz_pixmap* grayPix = fz_convert_pixmap(ctx, rgbPix, fz_device_gray(ctx), nullptr, nullptr, fz_default_color_params, 0);
        out.width = width;
        out.height = height;
        out.pixels.resize(static_cast<std::size_t>(width) * height);
        const unsigned char* samples = fz_pixmap_samples(ctx, grayPix);
        const int stride = fz_pixmap_stride(ctx, grayPix);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                out.pixels[static_cast<std::size_t>(y) * width + x] = samples[y * stride + x];
            }
        }
        fz_drop_pixmap(ctx, grayPix);
    }

    fz_drop_pixmap(ctx, rgbPix);
    fz_drop_context(ctx);
    return ok;
}

bool DocumentRenderer::renderPageRgba(const Page& page, const RenderOptions& options,
                                     std::vector<uint8_t>& outRgba, int& outWidth, int& outHeight,
                                     std::string& error, const Document* doc) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) {
        error = "could not create MuPDF context";
        return false;
    }
    fz_register_document_handlers(ctx);

    const float scale = options.scale > 0.05f ? options.scale : 1.0f;
    const int width = static_cast<int>(std::ceil(page.width() * scale));
    const int height = static_cast<int>(std::ceil(page.height() * scale));

    fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), width, height, nullptr, 1);
    fz_clear_pixmap_with_value(ctx, pix, 255);

    bool ok = renderPageToPixmap(ctx, page, options, pix, error, doc);
    if (ok) {
        outWidth = width;
        outHeight = height;
        outRgba.resize(static_cast<std::size_t>(width) * height * 4);
        const unsigned char* samples = fz_pixmap_samples(ctx, pix);
        const int stride = fz_pixmap_stride(ctx, pix);
        for (int y = 0; y < height; ++y) {
            std::memcpy(outRgba.data() + static_cast<std::size_t>(y) * width * 4, samples + static_cast<std::size_t>(y) * stride, width * 4);
        }
    }

    fz_drop_pixmap(ctx, pix);
    fz_drop_context(ctx);
    return ok;
}

bool DocumentRenderer::renderPageBase64Png(const Page& page, const RenderOptions& options,
                                          std::string& outBase64, int& outWidth, int& outHeight,
                                          std::string& error, const Document* doc) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) {
        error = "could not create MuPDF context";
        return false;
    }
    fz_register_document_handlers(ctx);

    const float scale = options.scale > 0.05f ? options.scale : 1.0f;
    const int width = static_cast<int>(std::ceil(page.width() * scale));
    const int height = static_cast<int>(std::ceil(page.height() * scale));

    fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), width, height, nullptr, 0);
    fz_clear_pixmap_with_value(ctx, pix, 255);

    bool ok = renderPageToPixmap(ctx, page, options, pix, error, doc);
    if (ok) {
        outWidth = width;
        outHeight = height;
        fz_buffer* buf = nullptr;
        fz_try(ctx) {
            buf = fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
            const unsigned char* data = nullptr;
            const std::size_t len = fz_buffer_storage(ctx, buf, const_cast<unsigned char**>(&data));
            outBase64 = base64Encode(data, len);
        }
        fz_always(ctx) {
            if (buf) fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            error = fz_caught_message(ctx);
            ok = false;
        }
    }

    fz_drop_pixmap(ctx, pix);
    fz_drop_context(ctx);
    return ok;
}

}  // namespace rpfg
