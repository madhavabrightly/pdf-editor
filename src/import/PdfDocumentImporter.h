#pragma once

#include <memory>
#include <string>
#include "document/Document.h"

namespace rpfg {

class PdfDocumentImporter {
public:
    PdfDocumentImporter();
    ~PdfDocumentImporter();

    // Imports a PDF file from disk into an authoritative DocumentModel
    std::shared_ptr<Document> importDocument(const std::string& path, std::string& error);

    // Imports a specific page index from a file into a Page model
    std::shared_ptr<Page> importPage(const std::string& path, int pageIndex, std::string& error);
};

}  // namespace rpfg
