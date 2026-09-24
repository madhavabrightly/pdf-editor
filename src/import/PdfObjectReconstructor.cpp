#include "import/PdfObjectReconstructor.h"

#include <cmath>
#include "document/ImageObject.h"
#include "document/LineObject.h"
#include "document/ProtectedObject.h"
#include "document/ShapeObject.h"
#include "document/VectorObject.h"

namespace rpfg {
namespace {

struct PathCollector {
    std::vector<PathCommand> commands;
    Matrix transform;
};

void walkMoveTo(fz_context* ctx, void* arg, float x, float y) {
    (void)ctx;
    auto* coll = static_cast<PathCollector*>(arg);
    const Point pt = coll->transform.transform(Point{x, y});
    PathCommand cmd;
    cmd.verb = PathVerb::MoveTo;
    cmd.p0 = pt;
    coll->commands.push_back(cmd);
}

void walkLineTo(fz_context* ctx, void* arg, float x, float y) {
    (void)ctx;
    auto* coll = static_cast<PathCollector*>(arg);
    const Point pt = coll->transform.transform(Point{x, y});
    PathCommand cmd;
    cmd.verb = PathVerb::LineTo;
    cmd.p0 = pt;
    coll->commands.push_back(cmd);
}

void walkCurveTo(fz_context* ctx, void* arg, float x1, float y1, float x2, float y2, float x3, float y3) {
    (void)ctx;
    auto* coll = static_cast<PathCollector*>(arg);
    PathCommand cmd;
    cmd.verb = PathVerb::CubicTo;
    cmd.p0 = coll->transform.transform(Point{x1, y1});
    cmd.p1 = coll->transform.transform(Point{x2, y2});
    cmd.p2 = coll->transform.transform(Point{x3, y3});
    coll->commands.push_back(cmd);
}

void walkClosePath(fz_context* ctx, void* arg) {
    (void)ctx;
    auto* coll = static_cast<PathCollector*>(arg);
    PathCommand cmd;
    cmd.verb = PathVerb::Close;
    coll->commands.push_back(cmd);
}

Matrix fromFzMatrix(const fz_matrix& m) {
    Matrix out;
    out.a = m.a;
    out.b = m.b;
    out.c = m.c;
    out.d = m.d;
    out.e = m.e;
    out.f = m.f;
    return out;
}

Color convertFzColor(fz_context* ctx, fz_colorspace* cs, const float* color, float alpha, fz_color_params params) {
    if (cs == nullptr || color == nullptr) {
        return Color{0.0f, 0.0f, 0.0f, alpha};
    }
    float rgb[FZ_MAX_COLORS] = {0};
    fz_convert_color(ctx, cs, color, fz_device_rgb(ctx), rgb, nullptr, params);
    return Color{rgb[0], rgb[1], rgb[2], alpha};
}

struct ClipEntry {
    Rect scissor;
    std::vector<PathCommand> commands;
};

struct ObjectExtractDevice {
    fz_device base;
    PageExtractionResult* result;
    std::vector<ClipEntry> clipStack;
};

void pushScissorClip(ObjectExtractDevice* d, Rect s, std::vector<PathCommand> commands = {}) {
    if (!d->clipStack.empty()) {
        s = d->clipStack.back().scissor.intersected(s);
    }
    ClipEntry entry;
    entry.scissor = s;
    entry.commands = std::move(commands);
    d->clipStack.push_back(std::move(entry));
}

void attachClip(ObjectExtractDevice* d, const std::shared_ptr<DocumentObject>& obj) {
    if (!d || d->clipStack.empty() || !obj) return;
    obj->setClipRect(d->clipStack.back().scissor);
    for (auto it = d->clipStack.rbegin(); it != d->clipStack.rend(); ++it) {
        if (!it->commands.empty()) {
            obj->setClipPath(it->commands);
            break;
        }
    }
}

void objClipPath(fz_context* ctx, fz_device* dev, const fz_path* path, int evenOdd, fz_matrix ctm, fz_rect scissor) {
    (void)evenOdd;
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d) return;

    fz_path_walker walker{};
    walker.moveto = walkMoveTo;
    walker.lineto = walkLineTo;
    walker.curveto = walkCurveTo;
    walker.closepath = walkClosePath;

    PathCollector coll;
    coll.transform = fromFzMatrix(ctm);
    fz_walk_path(ctx, path, &walker, &coll);

    pushScissorClip(d, Rect{scissor.x0, scissor.y0, scissor.x1, scissor.y1}, std::move(coll.commands));
}

void objClipStrokePath(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke, fz_matrix ctm, fz_rect scissor) {
    (void)ctx;
    (void)path;
    (void)stroke;
    (void)ctm;
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d) return;
    pushScissorClip(d, Rect{scissor.x0, scissor.y0, scissor.x1, scissor.y1});
}

void objClipText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_rect scissor) {
    (void)ctx;
    (void)text;
    (void)ctm;
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d) return;
    pushScissorClip(d, Rect{scissor.x0, scissor.y0, scissor.x1, scissor.y1});
}

void objClipStrokeText(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke, fz_matrix ctm, fz_rect scissor) {
    (void)ctx;
    (void)text;
    (void)stroke;
    (void)ctm;
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d) return;
    pushScissorClip(d, Rect{scissor.x0, scissor.y0, scissor.x1, scissor.y1});
}

void objClipImageMask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, fz_rect scissor) {
    (void)ctx;
    (void)image;
    (void)ctm;
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d) return;
    pushScissorClip(d, Rect{scissor.x0, scissor.y0, scissor.x1, scissor.y1});
}

void objPopClip(fz_context* ctx, fz_device* dev) {
    (void)ctx;
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d || d->clipStack.empty()) return;
    d->clipStack.pop_back();
}

void objFillPath(fz_context* ctx, fz_device* dev, const fz_path* path, int evenOdd, fz_matrix ctm,
                 fz_colorspace* cs, const float* color, float alpha, fz_color_params params) {
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d || !d->result || d->result->nonTextObjects.size() >= 3000) return;

    fz_path_walker walker{};
    walker.moveto = walkMoveTo;
    walker.lineto = walkLineTo;
    walker.curveto = walkCurveTo;
    walker.closepath = walkClosePath;

    PathCollector coll;
    coll.transform = fromFzMatrix(ctm);
    fz_walk_path(ctx, path, &walker, &coll);

    if (coll.commands.empty()) return;

    Color fillCol = convertFzColor(ctx, cs, color, alpha, params);

    // Check if it's an axis-aligned rectangle (4 lines + close)
    if (coll.commands.size() == 5 &&
        coll.commands[0].verb == PathVerb::MoveTo &&
        coll.commands[1].verb == PathVerb::LineTo &&
        coll.commands[2].verb == PathVerb::LineTo &&
        coll.commands[3].verb == PathVerb::LineTo &&
        coll.commands[4].verb == PathVerb::Close) {
        float x0 = std::min({coll.commands[0].p0.x, coll.commands[1].p0.x, coll.commands[2].p0.x, coll.commands[3].p0.x});
        float x1 = std::max({coll.commands[0].p0.x, coll.commands[1].p0.x, coll.commands[2].p0.x, coll.commands[3].p0.x});
        float y0 = std::min({coll.commands[0].p0.y, coll.commands[1].p0.y, coll.commands[2].p0.y, coll.commands[3].p0.y});
        float y1 = std::max({coll.commands[0].p0.y, coll.commands[1].p0.y, coll.commands[2].p0.y, coll.commands[3].p0.y});
        auto shape = std::make_shared<ShapeObject>(ShapeType::Rectangle, Rect{x0, y0, x1, y1});
        shape->setFillColor(fillCol);
        shape->setFilled(true);
        attachClip(d, shape);
        d->result->nonTextObjects.push_back(shape);
        return;
    }

    auto vec = std::make_shared<VectorObject>(std::move(coll.commands));
    vec->setFilled(true);
    vec->setStroked(false);
    vec->setFillColor(fillCol);
    vec->setEvenOdd(evenOdd != 0);
    attachClip(d, vec);
    d->result->nonTextObjects.push_back(vec);
}

void objStrokePath(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                   fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha, fz_color_params params) {
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d || !d->result || d->result->nonTextObjects.size() >= 3000) return;

    fz_path_walker walker{};
    walker.moveto = walkMoveTo;
    walker.lineto = walkLineTo;
    walker.curveto = walkCurveTo;
    walker.closepath = walkClosePath;

    PathCollector coll;
    coll.transform = fromFzMatrix(ctm);
    fz_walk_path(ctx, path, &walker, &coll);

    if (coll.commands.empty()) return;

    Color strokeCol = convertFzColor(ctx, cs, color, alpha, params);
    float width = stroke != nullptr ? stroke->linewidth * std::sqrt(std::abs(ctm.a * ctm.d - ctm.b * ctm.c)) : 1.0f;
    if (width < 0.25f) width = 0.25f;

    // Check if it's a straight line
    if (coll.commands.size() == 2 &&
        coll.commands[0].verb == PathVerb::MoveTo &&
        coll.commands[1].verb == PathVerb::LineTo) {
        auto line = std::make_shared<LineObject>(coll.commands[0].p0, coll.commands[1].p0, strokeCol, width);
        attachClip(d, line);
        d->result->nonTextObjects.push_back(line);
        return;
    }

    auto vec = std::make_shared<VectorObject>(std::move(coll.commands));
    vec->setFilled(false);
    vec->setStroked(true);
    vec->setStrokeColor(strokeCol);
    vec->setStrokeWidth(width);
    attachClip(d, vec);
    d->result->nonTextObjects.push_back(vec);
}

void objFillImage(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha, fz_color_params params) {
    (void)alpha;
    (void)params;
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d || !d->result || image == nullptr) return;

    const fz_rect bounds = fz_transform_rect(fz_unit_rect, ctm);
    Rect r{bounds.x0, bounds.y0, bounds.x1, bounds.y1};
    std::printf("objFillImage: bounds=[%.1f, %.1f, %.1f, %.1f], clipStack depth=%zu\n",
                r.x0, r.y0, r.x1, r.y1, d->clipStack.size());
    if (!d->clipStack.empty()) {
        const auto& c = d->clipStack.back().scissor;
        std::printf("  Active clip scissor: [%.1f, %.1f, %.1f, %.1f]\n", c.x0, c.y0, c.x1, c.y1);
    }

    fz_pixmap* rawPix = nullptr;
    fz_try(ctx) {
        rawPix = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
    }
    fz_catch(ctx) {
        rawPix = nullptr;
    }
    if (rawPix == nullptr) {
        auto prot = std::make_shared<ProtectedObject>(r, "unextractable image", 0);
        d->result->nonTextObjects.push_back(prot);
        return;
    }

    fz_pixmap* pix = nullptr;
    fz_try(ctx) {
        pix = fz_convert_pixmap(ctx, rawPix, fz_device_rgb(ctx), nullptr, nullptr, fz_default_color_params, 1);
    }
    fz_catch(ctx) {
        pix = nullptr;
    }
    fz_drop_pixmap(ctx, rawPix);

    if (pix == nullptr) {
        auto prot = std::make_shared<ProtectedObject>(r, "unextractable image", 0);
        d->result->nonTextObjects.push_back(prot);
        return;
    }

    const int w = fz_pixmap_width(ctx, pix);
    const int h = fz_pixmap_height(ctx, pix);
    const int stride = fz_pixmap_stride(ctx, pix);
    const unsigned char* samples = fz_pixmap_samples(ctx, pix);
    const int n = fz_pixmap_components(ctx, pix);

    // Extract mask / SMask if available
    fz_pixmap* grayMask = nullptr;
    if (image->mask != nullptr) {
        fz_pixmap* maskPix = nullptr;
        fz_try(ctx) {
            maskPix = fz_get_pixmap_from_image(ctx, image->mask, nullptr, nullptr, nullptr, nullptr);
        }
        fz_catch(ctx) {
            maskPix = nullptr;
        }
        if (maskPix != nullptr) {
            std::printf("  image->mask found: w=%d, h=%d, n=%d, cs=%s, imagemask=%d\n",
                        maskPix->w, maskPix->h, maskPix->n,
                        maskPix->colorspace ? fz_colorspace_name(ctx, maskPix->colorspace) : "null",
                        image->mask->imagemask);
            if (maskPix->colorspace == fz_device_gray(ctx) && maskPix->n == 1) {
                grayMask = maskPix;
            } else {
                fz_try(ctx) {
                    grayMask = fz_convert_pixmap(ctx, maskPix, fz_device_gray(ctx), nullptr, nullptr, fz_default_color_params, 0);
                }
                fz_catch(ctx) {
                    grayMask = nullptr;
                }
                fz_drop_pixmap(ctx, maskPix);
            }
        }
    }

    const int mw = grayMask != nullptr ? fz_pixmap_width(ctx, grayMask) : 0;
    const int mh = grayMask != nullptr ? fz_pixmap_height(ctx, grayMask) : 0;
    const int mstride = grayMask != nullptr ? fz_pixmap_stride(ctx, grayMask) : 0;
    const unsigned char* msamples = grayMask != nullptr ? fz_pixmap_samples(ctx, grayMask) : nullptr;

    std::vector<uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t srcIdx = static_cast<std::size_t>(y) * stride + x * n;
            const std::size_t dstIdx = (static_cast<std::size_t>(y) * w + x) * 4;

            rgba[dstIdx + 0] = samples[srcIdx + 0];
            rgba[dstIdx + 1] = samples[srcIdx + 1];
            rgba[dstIdx + 2] = samples[srcIdx + 2];

            if (grayMask != nullptr) {
                int mx = (mw == w) ? x : std::clamp(static_cast<int>((x + 0.5f) * mw / w), 0, mw - 1);
                int my = (mh == h) ? y : std::clamp(static_cast<int>((y + 0.5f) * mh / h), 0, mh - 1);
                rgba[dstIdx + 3] = msamples[static_cast<std::size_t>(my) * mstride + mx];
            } else if (n >= 4) {
                rgba[dstIdx + 3] = samples[srcIdx + 3];
            } else {
                rgba[dstIdx + 3] = 255;
            }
        }
    }

    if (grayMask != nullptr) {
        fz_drop_pixmap(ctx, grayMask);
    }

    if (image->use_colorkey && grayMask == nullptr) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t dstIdx = (static_cast<std::size_t>(y) * w + x) * 4;
                uint8_t r_v = rgba[dstIdx + 0];
                uint8_t g_v = rgba[dstIdx + 1];
                uint8_t b_v = rgba[dstIdx + 2];
                if (r_v >= image->colorkey[0] && r_v <= image->colorkey[1] &&
                    g_v >= image->colorkey[2] && g_v <= image->colorkey[3] &&
                    b_v >= image->colorkey[4] && b_v <= image->colorkey[5]) {
                    rgba[dstIdx + 3] = 0;
                }
            }
        }
    }

    fz_drop_pixmap(ctx, pix);

    auto imgObj = std::make_shared<ImageObject>(r, w, h, ImageFormat::RGBA, std::move(rgba));
    imgObj->setTransform(fromFzMatrix(ctm));
    attachClip(d, imgObj);
    d->result->nonTextObjects.push_back(imgObj);
}

void objFillShade(fz_context* ctx, fz_device* dev, fz_shade* shade, fz_matrix ctm, float alpha, fz_color_params params) {
    (void)alpha;
    (void)params;
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d || !d->result || shade == nullptr) return;

    const fz_rect b = fz_bound_shade(ctx, shade, ctm);
    auto prot = std::make_shared<ProtectedObject>(Rect{b.x0, b.y0, b.x1, b.y1}, "smooth shading pattern", 0);
    attachClip(d, prot);
    d->result->nonTextObjects.push_back(prot);
}

void objFillText(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                 fz_colorspace* cs, const float* color, float alpha, fz_color_params params) {
    auto* d = reinterpret_cast<ObjectExtractDevice*>(dev);
    if (!d || !d->result || text == nullptr) return;

    Color col = convertFzColor(ctx, cs, color, alpha, params);

    for (const fz_text_span* span = text->head; span != nullptr; span = span->next) {
        if (span->len <= 0) continue;
        const fz_point pt = fz_transform_point(fz_make_point(span->items[0].x, span->items[0].y), ctm);

        TextSpanProbe probe;
        probe.x = pt.x;
        probe.y = pt.y;
        probe.r = col.r;
        probe.g = col.g;
        probe.b = col.b;
        const char* name = span->font != nullptr ? fz_font_name(ctx, span->font) : nullptr;
        probe.fontName = name != nullptr ? name : "Helvetica";
        if (span->font != nullptr) {
            probe.isBold = fz_font_is_bold(ctx, span->font) != 0;
            probe.isItalic = fz_font_is_italic(ctx, span->font) != 0;
        }
        d->result->textProbes.push_back(std::move(probe));
    }
}

}  // namespace

PdfObjectReconstructor::PdfObjectReconstructor() = default;

PageExtractionResult PdfObjectReconstructor::extractObjects(fz_context* ctx, fz_display_list* list) {
    PageExtractionResult result;
    if (ctx == nullptr || list == nullptr) {
        return result;
    }

    auto* dev = fz_new_derived_device(ctx, ObjectExtractDevice);
    dev->result = &result;
    dev->base.fill_path = objFillPath;
    dev->base.stroke_path = objStrokePath;
    dev->base.clip_path = objClipPath;
    dev->base.clip_stroke_path = objClipStrokePath;
    dev->base.clip_text = objClipText;
    dev->base.clip_stroke_text = objClipStrokeText;
    dev->base.clip_image_mask = objClipImageMask;
    dev->base.pop_clip = objPopClip;
    dev->base.fill_image = objFillImage;
    dev->base.fill_shade = objFillShade;
    dev->base.fill_text = objFillText;

    fz_try(ctx) {
        fz_run_display_list(ctx, list, &dev->base, fz_identity, fz_infinite_rect, nullptr);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, &dev->base);
    }
    fz_catch(ctx) {
        // Fallback or partial on error
    }

    return result;
}

}  // namespace rpfg
