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

class Document;
class Page;

// Writes `model` as a fresh one-page PDF at `path`. The text is laid out by the
// font engine (fz_show_string), so advances, kerning and spaces are real, and
// each word's face/size/colour is taken from the reconstructed document.
bool writeDocumentPdf(const PageModel& model, const std::string& path, std::string& error);

// Writes a single Page to `path`. If `doc` is provided, embedded fonts from the document are used.
// Re-emits all shapes, lines, vector paths, images, and text frames.
bool writeDocumentPdf(const Page& page, const std::string& path, std::string& error, const Document* doc = nullptr);

// Writes the complete authoritative Document to `path`.
bool writeDocumentPdf(const Document& doc, const std::string& path, std::string& error);

}  // namespace rpfg
