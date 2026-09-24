#include "import/PdfDocumentImporter.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include "import/PdfObjectReconstructor.h"
#include "import/PdfTextReconstructor.h"
#include "util/Logger.h"

namespace rpfg {

PdfDocumentImporter::PdfDocumentImporter() = default;
PdfDocumentImporter::~PdfDocumentImporter() = default;

std::shared_ptr<Document> PdfDocumentImporter::importDocument(const std::string& path, std::string& error) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (ctx == nullptr) {
        error = "could not create MuPDF context for import";
        return nullptr;
    }
    fz_register_document_handlers(ctx);

    fz_document* fzDoc = nullptr;
    int pageCount = 0;

    fz_try(ctx) {
        fzDoc = fz_open_document(ctx, path.c_str());
        pageCount = fz_count_pages(ctx, fzDoc);
    }
    fz_catch(ctx) {
        error = fz_caught_message(ctx);
        fzDoc = nullptr;
    }

    if (fzDoc == nullptr) {
        fz_drop_context(ctx);
        return nullptr;
    }

    auto doc = std::make_shared<Document>();
    doc->setSourcePath(path);

    PdfObjectReconstructor objReconstructor;
    PdfTextReconstructor textReconstructor;

    for (int i = 0; i < pageCount; ++i) {
        fz_page* page = nullptr;
        fz_display_list* list = nullptr;
        fz_stext_page* stext = nullptr;
        fz_rect bounds{};

        fz_try(ctx) {
            page = fz_load_page(ctx, fzDoc, i);
            bounds = fz_bound_page(ctx, page);
            list = fz_new_display_list_from_page(ctx, page);
            fz_stext_options options{};
            fz_init_stext_options(ctx, &options);
            stext = fz_new_stext_page_from_display_list(ctx, list, &options);
        }
        fz_catch(ctx) {
            // Keep going if one page has issues
        }

        const float width = bounds.x1 - bounds.x0;
        const float height = bounds.y1 - bounds.y0;
        auto pageModel = std::make_shared<Page>(width > 0.0f ? width : 612.0f,
                                                height > 0.0f ? height : 792.0f, i);

        if (list != nullptr) {
            PageExtractionResult pageObjResult = objReconstructor.extractObjects(ctx, list);

            // Add non-text objects (lines, shapes, vectors, images, protected)
            for (auto& obj : pageObjResult.nonTextObjects) {
                pageModel->addObject(std::move(obj));
            }

            // Reconstruct text frames
            if (stext != nullptr) {
                auto textFrames = textReconstructor.reconstructFrames(
                    ctx, stext, pageObjResult.textProbes, pageModel->width(), pageModel->height(), doc.get());
                for (auto& tf : textFrames) {
                    pageModel->addObject(std::move(tf));
                }
            }
        }

        doc->addPage(std::move(pageModel));

        if (stext != nullptr) {
            fz_drop_stext_page(ctx, stext);
        }
        if (list != nullptr) {
            fz_drop_display_list(ctx, list);
        }
        if (page != nullptr) {
            fz_drop_page(ctx, page);
        }
    }

    doc->clearDirty();

    fz_try(ctx) {
        fz_drop_document(ctx, fzDoc);
    }
    fz_catch(ctx) {}

    fz_drop_context(ctx);
    return doc;
}

std::shared_ptr<Page> PdfDocumentImporter::importPage(const std::string& path, int pageIndex, std::string& error) {
    auto doc = importDocument(path, error);
    if (!doc || pageIndex >= static_cast<int>(doc->pageCount())) {
        return nullptr;
    }
    return doc->page(static_cast<std::size_t>(pageIndex));
}

}  // namespace rpfg
