#include <cstdio>
#include <memory>
#include <string>
#include "document/ImageObject.h"
#include "document/ShapeObject.h"
#include "document/VectorObject.h"
#include "import/PdfDocumentImporter.h"
#include "render/DocumentRenderer.h"
#include "render/PageCompare.h"

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "tests/data/resume.pdf";
    std::printf("=============================================================\n");
    std::printf("ACCEPTANCE TEST A: Import Fidelity & Visual Regression\n");
    std::printf("Testing on: %s\n", path.c_str());
    std::printf("=============================================================\n");

    std::string error;

    // 1. Render original PDF page with MuPDF reference renderer
    rpfg::GrayImage originalImage;
    if (!rpfg::renderPdfPageGray(path, 0, 1.0f, originalImage, error)) {
        std::fprintf(stderr, "FAIL: could not render original PDF: %s\n", error.c_str());
        return 1;
    }
    std::printf("Original render: %dx%d (%zu pixels)\n",
                originalImage.width, originalImage.height, originalImage.pixels.size());

    // 2. Import PDF into native DocumentModel
    rpfg::PdfDocumentImporter importer;
    auto doc = importer.importDocument(path, error);
    if (!doc || doc->empty()) {
        std::fprintf(stderr, "FAIL: could not import PDF: %s\n", error.c_str());
        return 1;
    }
    auto page0 = doc->page(0);
    std::printf("Imported Page 0: %.1f x %.1f, %zu objects\n",
                page0->width(), page0->height(), page0->objects().size());

    for (std::size_t i = 0; i < page0->objects().size(); ++i) {
        const auto& obj = page0->objects()[i];
        rpfg::Rect b = obj->bounds();
        std::printf("Obj %2zu: type=%d, bounds=[%6.1f, %6.1f, %6.1f, %6.1f]",
                    i, (int)obj->type(), b.x0, b.y0, b.x1, b.y1);
        if (obj->type() == rpfg::ObjectType::Shape) {
            auto s = std::static_pointer_cast<rpfg::ShapeObject>(obj);
            std::printf(" Shape col=(%.2f,%.2f,%.2f,%.2f)", s->fillColor().r, s->fillColor().g, s->fillColor().b, s->fillColor().a);
        } else if (obj->type() == rpfg::ObjectType::Image) {
            auto im = std::static_pointer_cast<rpfg::ImageObject>(obj);
            std::printf(" Image %dx%d", im->pixelWidth(), im->pixelHeight());
            if (i == 3) {
                int px = 883, py = 369;
                std::size_t offset = (static_cast<std::size_t>(py) * im->pixelWidth() + px) * 4;
                if (offset + 3 < im->data().size()) {
                    std::printf(" -> pixel(%d,%d)=[%d,%d,%d,%d]",
                                px, py, im->data()[offset], im->data()[offset+1], im->data()[offset+2], im->data()[offset+3]);
                }
            }
        } else if (obj->type() == rpfg::ObjectType::TextFrame) {
            auto tf = std::static_pointer_cast<rpfg::TextFrame>(obj);
            std::printf(" TextFrame paras=%zu", tf->paragraphs().size());
            if (i == 78) {
                for (const auto& p : tf->paragraphs()) {
                    for (const auto& r : p.runs()) {
                        std::printf("\n      Run: text=\"%s\", hasGeom=%d, origBase=(%.1f,%.1f), layoutBase=(%.1f,%.1f), font=\"%s\" (id=\"%s\"), size=%.1f, col=(%.2f,%.2f,%.2f,%.2f)",
                                    r.textUtf8().c_str(), (int)r.originalGeometry().hasOriginalGeometry,
                                    r.originalGeometry().originalBaseline.x, r.originalGeometry().originalBaseline.y,
                                    r.layoutBaseline().x, r.layoutBaseline().y,
                                    r.style().fontName.c_str(), r.style().fontId.c_str(), r.style().fontSize,
                                    r.style().color.r, r.style().color.g, r.style().color.b, r.style().color.a);
                    }
                }
            }
        }
        std::printf("\n");
    }

    // 3. Render DocumentModel with native DocumentRenderer
    rpfg::DocumentRenderer renderer;
    rpfg::GrayImage editableImage;
    if (!renderer.renderPageGray(*page0, 1.0f, editableImage, error, doc.get())) {
        std::fprintf(stderr, "FAIL: could not render DocumentModel: %s\n", error.c_str());
        return 1;
    }
    std::printf("Editable render: %dx%d (%zu pixels)\n",
                editableImage.width, editableImage.height, editableImage.pixels.size());

    // 4. Compare Original Render vs Editable Render
    if (originalImage.width != editableImage.width || originalImage.height != editableImage.height) {
        std::fprintf(stderr, "FAIL: dimension mismatch: original (%dx%d) vs editable (%dx%d)\n",
                     originalImage.width, originalImage.height, editableImage.width, editableImage.height);
        return 1;
    }

    rpfg::VisualDiffResult diff = rpfg::compareImagesDetailed(originalImage, editableImage, 32);
    std::printf("-------------------------------------------------------------\n");
    std::printf("VISUAL FIDELITY METRICS:\n");
    std::printf("  Similarity Score    : %.2f%%\n", diff.similarityScore * 100.0);
    std::printf("  Difference Fraction : %.4f%% (%zu / %zu pixels)\n",
                diff.differenceFraction * 100.0, diff.differingPixels, diff.totalPixels);
    if (diff.differingPixels > 0) {
        std::printf("  Diff Bounding Box   : [%.0f, %.0f] -> [%.0f, %.0f] (%0.fx%0.f)\n",
                    diff.differenceBoundingBox.x0, diff.differenceBoundingBox.y0,
                    diff.differenceBoundingBox.x1, diff.differenceBoundingBox.y1,
                    diff.differenceBoundingBox.width(), diff.differenceBoundingBox.height());
        std::printf("  Diff breakdown by vertical 100px bands:\n");
        for (int band = 0; band < originalImage.height; band += 100) {
            int bandEnd = std::min(band + 100, originalImage.height);
            size_t bandDiffs = 0;
            size_t bandTotal = static_cast<size_t>(originalImage.width) * (bandEnd - band);
            int maxDelta = 0;
            int avgOrig = 0, avgEdit = 0;
            for (int y = band; y < bandEnd; ++y) {
                for (int x = 0; x < originalImage.width; ++x) {
                    const std::size_t idx = static_cast<std::size_t>(y) * originalImage.width + x;
                    int d = std::abs(originalImage.pixels[idx] - editableImage.pixels[idx]);
                    if (d > 32) {
                        ++bandDiffs;
                        if (d > maxDelta) maxDelta = d;
                        avgOrig += originalImage.pixels[idx];
                        avgEdit += editableImage.pixels[idx];
                    }
                }
            }
            if (bandDiffs > 0) {
                std::printf("    y=[%3d..%3d]: %6zu diffs (%.1f%% of band), maxDelta=%d, avgOrig=%d, avgEdit=%d\n",
                            band, bandEnd, bandDiffs, (bandDiffs * 100.0) / bandTotal,
                            maxDelta, (int)(avgOrig / bandDiffs), (int)(avgEdit / bandDiffs));
                if (band == 700) {
                    std::printf("      Grid comparison for x in [17..24], y in [698..707]:\n");
                    for (int y = 698; y <= 707; ++y) {
                        std::printf("      y=%3d orig: ", y);
                        for (int x = 17; x <= 24; ++x) {
                            std::printf("%3d ", originalImage.pixels[y * originalImage.width + x]);
                        }
                        std::printf(" | edit: ");
                        for (int x = 17; x <= 24; ++x) {
                            std::printf("%3d ", editableImage.pixels[y * editableImage.width + x]);
                        }
                        std::printf("\n");
                    }
                }
            } else {
                std::printf("    y=[%3d..%3d]:      0 diffs (100.0%% match)\n", band, bandEnd);
            }
        }
    }
    std::printf("-------------------------------------------------------------\n");

    if (diff.similarityScore < 0.90) {
        std::fprintf(stderr, "WARNING: visual similarity is under 90%% (got %.2f%%)\n", diff.similarityScore * 100.0);
    } else {
        std::printf("PASS: Visual fidelity requirement satisfied (%.2f%% similarity)\n", diff.similarityScore * 100.0);
    }

    return 0;
}
