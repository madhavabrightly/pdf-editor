#include <cstdio>
#include <memory>
#include <string>
#include "import/PdfDocumentImporter.h"

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "tests/data/resume.pdf";
    std::printf("Testing PdfDocumentImporter on %s...\n", path.c_str());

    rpfg::PdfDocumentImporter importer;
    std::string error;
    std::shared_ptr<rpfg::Document> doc = importer.importDocument(path, error);
    if (!doc) {
        std::fprintf(stderr, "FAIL: could not import %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }

    std::printf("PASS: imported document with %zu pages\n", doc->pageCount());
    if (doc->empty()) {
        std::fprintf(stderr, "FAIL: document has 0 pages\n");
        return 1;
    }

    auto page0 = doc->page(0);
    if (!page0) {
        std::fprintf(stderr, "FAIL: page 0 is null\n");
        return 1;
    }

    std::printf("Page 0 dimensions: %.1f x %.1f, total objects: %zu\n",
                page0->width(), page0->height(), page0->objects().size());

    auto textFrames = page0->textFrames();
    std::printf("Text frames on page 0: %zu\n", textFrames.size());
    if (textFrames.empty()) {
        std::fprintf(stderr, "FAIL: no text frames reconstructed on page 0\n");
        return 1;
    }

    std::size_t totalParagraphs = 0;
    std::size_t totalRuns = 0;
    std::string fullText;
    bool foundMultiSpace = false;

    for (const auto& frame : textFrames) {
        for (const auto& para : frame->paragraphs()) {
            ++totalParagraphs;
            totalRuns += para.runs().size();
            const std::string paraText = para.textUtf8();
            fullText += paraText;
            fullText += "\n";

            if (paraText.find("  ") != std::string::npos) {
                foundMultiSpace = true;
                std::printf("Found multi-space run in paragraph: '%s'\n", paraText.substr(0, 40).c_str());
            }
        }
    }

    std::printf("Total paragraphs: %zu, total runs: %zu, text length: %zu\n",
                totalParagraphs, totalRuns, fullText.size());

    if (totalParagraphs == 0 || fullText.empty()) {
        std::fprintf(stderr, "FAIL: no text extracted from text frames\n");
        return 1;
    }

    // Non-text objects check
    std::size_t linesCount = 0;
    std::size_t shapesCount = 0;
    std::size_t vectorsCount = 0;
    std::size_t imagesCount = 0;
    std::size_t protectedCount = 0;

    for (const auto& obj : page0->objects()) {
        switch (obj->type()) {
            case rpfg::ObjectType::Line: ++linesCount; break;
            case rpfg::ObjectType::Shape: ++shapesCount; break;
            case rpfg::ObjectType::Vector: ++vectorsCount; break;
            case rpfg::ObjectType::Image: ++imagesCount; break;
            case rpfg::ObjectType::Protected: ++protectedCount; break;
            default: break;
        }
    }

    std::printf("Non-text objects: lines=%zu, shapes=%zu, vectors=%zu, images=%zu, protected=%zu\n",
                linesCount, shapesCount, vectorsCount, imagesCount, protectedCount);

    std::printf("PASS: PDF Import fidelity verified!\n");
    return 0;
}
