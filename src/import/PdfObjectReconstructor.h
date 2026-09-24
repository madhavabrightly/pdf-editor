#pragma once

#include <memory>
#include <vector>
#include <mupdf/fitz.h>
#include "document/DocumentObject.h"
#include "import/PdfTextReconstructor.h"

namespace rpfg {

struct PageExtractionResult {
    std::vector<std::shared_ptr<DocumentObject>> nonTextObjects;
    std::vector<TextSpanProbe> textProbes;
};

class PdfObjectReconstructor {
public:
    PdfObjectReconstructor();

    PageExtractionResult extractObjects(fz_context* ctx, fz_display_list* list);
};

}  // namespace rpfg
