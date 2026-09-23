#include "render/PageCompare.h"

#include <mupdf/fitz.h>

#include <cmath>

namespace rpfg {

bool renderPdfPageGray(const std::string& path, int pageIndex, float scale, GrayImage& out,
                       std::string& error) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (ctx == nullptr) {
        error = "could not create a MuPDF context";
        return false;
    }
    fz_register_document_handlers(ctx);

    fz_document* document = nullptr;
    fz_page* page = nullptr;
    fz_pixmap* pixmap = nullptr;
    bool ok = false;

    fz_try(ctx) {
        document = fz_open_document(ctx, path.c_str());
        page = fz_load_page(ctx, document, pageIndex);
        const fz_matrix ctm = fz_scale(scale, scale);
        pixmap = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_gray(ctx), 0);

        const int width = fz_pixmap_width(ctx, pixmap);
        const int height = fz_pixmap_height(ctx, pixmap);
        const int stride = fz_pixmap_stride(ctx, pixmap);
        const unsigned char* samples = fz_pixmap_samples(ctx, pixmap);

        out.width = width;
        out.height = height;
        out.pixels.resize(static_cast<std::size_t>(width) * height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                out.pixels[static_cast<std::size_t>(y) * width + x] = samples[y * stride + x];
            }
        }
        ok = true;
    }
    fz_catch(ctx) {
        error = fz_caught_message(ctx);
        ok = false;
    }

    if (pixmap != nullptr) {
        fz_drop_pixmap(ctx, pixmap);
    }
    if (page != nullptr) {
        fz_drop_page(ctx, page);
    }
    if (document != nullptr) {
        fz_drop_document(ctx, document);
    }
    fz_drop_context(ctx);
    return ok;
}

int pdfPageCount(const std::string& path) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (ctx == nullptr) {
        return 0;
    }
    fz_register_document_handlers(ctx);

    fz_document* document = nullptr;
    int count = 0;
    fz_try(ctx) {
        document = fz_open_document(ctx, path.c_str());
        count = fz_count_pages(ctx, document);
    }
    fz_catch(ctx) {
        count = 0;
    }
    if (document != nullptr) {
        fz_drop_document(ctx, document);
    }
    fz_drop_context(ctx);
    return count;
}

double imageDifferenceFraction(const GrayImage& a, const GrayImage& b, int tolerance) {
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size()) {
        return 1.0;
    }
    if (a.pixels.empty()) {
        return 0.0;
    }
    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        if (std::abs(static_cast<int>(a.pixels[i]) - static_cast<int>(b.pixels[i])) > tolerance) {
            ++differing;
        }
    }
    return static_cast<double>(differing) / static_cast<double>(a.pixels.size());
}

}  // namespace rpfg
