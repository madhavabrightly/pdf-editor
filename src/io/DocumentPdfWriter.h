// Document -> PDF export.
//
// This is the "PDF writer" of the pipeline: the editor's source of truth is the
// DocumentModel, and saving means writing a *real* PDF from it - real text
// operators, real fonts, searchable and selectable - not a screenshot and not a
// patch of the original file.

#pragma once

#include <string>

#include "document/TextModel.h"

namespace rpfg {

// Writes `model` as a fresh one-page PDF at `path`. The text is laid out by the
// font engine (fz_show_string), so advances, kerning and spaces are real, and
// each word's face/size/colour is taken from the reconstructed document.
bool writeDocumentPdf(const PageModel& model, const std::string& path, std::string& error);

}  // namespace rpfg
