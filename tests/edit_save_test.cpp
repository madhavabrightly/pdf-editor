// End-to-end check for the edit-and-save path: open a PDF, replace a run's text
// in place, save it (both full and incremental), reopen the result and confirm
// the old text is gone and the new text is there. Text extraction is the oracle:
// after a redaction the removed glyphs must not come back, so a passing test
// means the content stream really was rewritten, not just covered up.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pdf/PdfEngine.h"

namespace {

std::mutex g_mutex;
std::condition_variable g_ready;
std::vector<nlohmann::json> g_pages;

void resetCollector() {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_pages.clear();
}

void collectPage(std::shared_ptr<const rpfg::PageText> text) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_pages.push_back(nlohmann::json::parse(*text->runs));
    g_ready.notify_all();
}

// Extracts one page through a fresh engine and returns the concatenated text.
std::optional<std::string> extractPage(const std::string& path, int index) {
    resetCollector();

    rpfg::PdfEngine engine;
    engine.setPageTextCallback(collectPage);

    std::string error;
    if (!engine.openDocument(path, error).has_value()) {
        std::fprintf(stderr, "FAIL: could not open %s: %s\n", path.c_str(), error.c_str());
        return std::nullopt;
    }
    engine.requestViewport({index}, 1.0f);

    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_ready.wait_for(lock, std::chrono::seconds(60), [] { return !g_pages.empty(); })) {
        std::fprintf(stderr, "FAIL: no text arrived for %s\n", path.c_str());
        return std::nullopt;
    }

    std::string text;
    for (const auto& run : g_pages.front()["runs"]) {
        text += run["t"].get<std::string>();
        text += "\n";
    }
    return text;
}

// Opens the source, replaces the first run containing `from` with `to`, and saves.
bool editAndSave(const std::string& source, const std::string& destination, rpfg::SaveMode mode,
                 const std::string& from, const std::string& to, const std::string& fontId = "",
                 float r = 0.0f, float g = 0.0f, float b = 0.0f) {
    resetCollector();

    rpfg::PdfEngine engine;
    engine.setPageTextCallback(collectPage);

    std::string error;
    if (!engine.openDocument(source, error).has_value()) {
        std::fprintf(stderr, "FAIL: could not open %s: %s\n", source.c_str(), error.c_str());
        return false;
    }
    engine.requestViewport({0}, 1.0f);

    nlohmann::json target;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_ready.wait_for(lock, std::chrono::seconds(60), [] { return !g_pages.empty(); })) {
            std::fprintf(stderr, "FAIL: no text arrived for %s\n", source.c_str());
            return false;
        }
        for (const auto& run : g_pages.front()["runs"]) {
            if (run["t"].get<std::string>().find(from) != std::string::npos) {
                target = run;
                break;
            }
        }
    }
    if (target.is_null()) {
        std::fprintf(stderr, "FAIL: no run containing '%s'\n", from.c_str());
        return false;
    }

    rpfg::TextEdit edit;
    edit.page = 0;
    edit.x = target["x"].get<float>();
    edit.y = target["y"].get<float>();
    edit.width = target["w"].get<float>();
    edit.size = target["s"].get<float>();
    edit.angle = target["a"].get<float>();
    edit.ascender = target["asc"].get<float>();
    edit.text = to;
    edit.font = fontId;
    edit.r = r;
    edit.g = g;
    edit.b = b;

    if (!engine.applyTextEdit(edit, error)) {
        std::fprintf(stderr, "FAIL: applyTextEdit: %s\n", error.c_str());
        return false;
    }
    if (!engine.hasUnsavedChanges()) {
        std::fprintf(stderr, "FAIL: engine did not report unsaved changes after an edit\n");
        return false;
    }
    if (!engine.saveDocument(destination, mode, error)) {
        std::fprintf(stderr, "FAIL: saveDocument: %s\n", error.c_str());
        return false;
    }
    if (engine.hasUnsavedChanges()) {
        std::fprintf(stderr, "FAIL: engine still dirty after saving\n");
        return false;
    }
    return true;
}

bool expectText(const std::string& path, int page, const std::string& needle, bool present) {
    const std::optional<std::string> text = extractPage(path, page);
    if (!text.has_value()) {
        return false;
    }
    const bool found = text->find(needle) != std::string::npos;
    if (found != present) {
        std::fprintf(stderr, "FAIL: '%s' %s in %s (page %d)\n", needle.c_str(),
                     present ? "is missing" : "is still present", path.c_str(), page + 1);
        std::fprintf(stderr, "---- extracted text ----\n%s------------------------\n", text->c_str());
        return false;
    }
    return true;
}

// Inserts new text (replace = false) and saves.
bool insertAndSave(const std::string& source, const std::string& destination,
                   const std::string& fontId, const std::string& text) {
    rpfg::PdfEngine engine;
    std::string error;
    if (!engine.openDocument(source, error).has_value()) {
        std::fprintf(stderr, "FAIL: could not open %s: %s\n", source.c_str(), error.c_str());
        return false;
    }

    rpfg::TextEdit edit;
    edit.page = 0;
    edit.replace = false;
    edit.x = 300.0f;
    edit.y = 400.0f;
    edit.size = 24.0f;
    edit.r = 1.0f;
    edit.g = 0.0f;
    edit.b = 0.0f;
    edit.font = fontId;
    edit.text = text;

    if (!engine.applyTextEdit(edit, error)) {
        std::fprintf(stderr, "FAIL: insert: %s\n", error.c_str());
        return false;
    }
    if (!engine.saveDocument(destination, rpfg::SaveMode::Full, error)) {
        std::fprintf(stderr, "FAIL: save after insert: %s\n", error.c_str());
        return false;
    }
    return true;
}

// Erases everything inside a page-space rectangle and saves.
bool eraseAndSave(const std::string& source, const std::string& destination, float x0, float y0,
                  float x1, float y1) {
    rpfg::PdfEngine engine;
    std::string error;
    if (!engine.openDocument(source, error).has_value()) {
        std::fprintf(stderr, "FAIL: could not open %s: %s\n", source.c_str(), error.c_str());
        return false;
    }
    if (!engine.eraseRegion(0, x0, y0, x1, y1, error)) {
        std::fprintf(stderr, "FAIL: eraseRegion: %s\n", error.c_str());
        return false;
    }
    if (!engine.saveDocument(destination, rpfg::SaveMode::Full, error)) {
        std::fprintf(stderr, "FAIL: save after erase: %s\n", error.c_str());
        return false;
    }
    return true;
}

// Replaces a two-line block (the heading and the line under it) and saves.
bool blockEditAndSave(const std::string& source, const std::string& destination,
                      const std::string& text, const std::string& fontId = "") {
    rpfg::PdfEngine engine;
    std::string error;
    if (!engine.openDocument(source, error).has_value()) {
        std::fprintf(stderr, "FAIL: could not open %s: %s\n", source.c_str(), error.c_str());
        return false;
    }

    rpfg::TextBlockEdit edit;
    edit.page = 0;
    edit.x0 = 80.0f;
    edit.y0 = 40.0f;
    edit.x1 = 400.0f;
    edit.y1 = 150.0f;
    edit.size = 14.0f;
    edit.font = fontId;
    edit.text = text;
    edit.lines.push_back(rpfg::TextLineBox{92.0f, 92.0f, 36.0f});
    edit.lines.push_back(rpfg::TextLineBox{92.0f, 132.0f, 14.0f});

    if (!engine.applyTextBlock(edit, error)) {
        std::fprintf(stderr, "FAIL: applyTextBlock: %s\n", error.c_str());
        return false;
    }
    if (!engine.saveDocument(destination, rpfg::SaveMode::Full, error)) {
        std::fprintf(stderr, "FAIL: save after block edit: %s\n", error.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rpfg_edit_test <file.pdf>\n");
        return 2;
    }

    namespace fs = std::filesystem;
    const std::string source = argv[1];
    const fs::path temp = fs::temp_directory_path();
    const std::string from = "Rpfg test - page 1";
    const std::string to = "Rpfg edited - page 1";

    // --- full save (Save As) -------------------------------------------------
    const std::string fullPath = (temp / "rpfg-edit-full.pdf").string();
    if (!editAndSave(source, fullPath, rpfg::SaveMode::Full, from, to)) {
        return 1;
    }
    if (!expectText(fullPath, 0, to, true) || !expectText(fullPath, 0, from, false)) {
        return 1;
    }
    std::printf("PASS: full save rewrote the text\n");

    // --- incremental save (Save) --------------------------------------------
    const std::string incPath = (temp / "rpfg-edit-incremental.pdf").string();
    fs::copy_file(source, incPath, fs::copy_options::overwrite_existing);
    if (!editAndSave(incPath, incPath, rpfg::SaveMode::Incremental, from, to)) {
        return 1;
    }
    if (!expectText(incPath, 0, to, true) || !expectText(incPath, 0, from, false)) {
        return 1;
    }
    std::printf("PASS: incremental save appended the edit\n");

    // --- an unrelated page survives untouched --------------------------------
    if (!expectText(fullPath, 1, "page 2", true)) {
        return 1;
    }
    std::printf("PASS: untouched page 2 text is intact\n");

    // --- the font library ----------------------------------------------------
    std::string embeddedFontId;
    {
        rpfg::PdfEngine engine;
        const std::vector<rpfg::FontInfo> fonts = engine.availableFonts();
        bool hasStandard = false;
        for (const rpfg::FontInfo& font : fonts) {
            if (!font.embedded) {
                hasStandard = true;
            } else if (embeddedFontId.empty()) {
                embeddedFontId = font.id;
            }
        }
        if (!hasStandard || fonts.size() < 3) {
            std::fprintf(stderr, "FAIL: font library looks wrong (%zu faces)\n", fonts.size());
            return 1;
        }
        std::printf("PASS: font library (%zu faces, %s)\n", fonts.size(),
                    embeddedFontId.empty() ? "no embeddable faces on this machine"
                                           : "embedded faces available");
    }

    // --- insert text with a standard font -----------------------------------
    {
        const std::string insertPath = (temp / "rpfg-insert.pdf").string();
        if (!insertAndSave(source, insertPath, "helv", "INSERTED-MARKER") ||
            !expectText(insertPath, 0, "INSERTED-MARKER", true)) {
            return 1;
        }
        std::printf("PASS: inserted text (standard font)\n");
    }

    // --- insert text with an embedded TrueType font -------------------------
    if (!embeddedFontId.empty()) {
        const std::string ttfPath = (temp / "rpfg-insert-ttf.pdf").string();
        if (!insertAndSave(source, ttfPath, embeddedFontId, "TTF-MARKER") ||
            !expectText(ttfPath, 0, "TTF-MARKER", true)) {
            return 1;
        }
        std::printf("PASS: inserted text (embedded font '%s')\n", embeddedFontId.c_str());
    } else {
        std::printf("SKIP: no embedded font available to test\n");
    }

    // --- replace using a chosen font and colour -----------------------------
    {
        const std::string styledPath = (temp / "rpfg-styled.pdf").string();
        if (!editAndSave(source, styledPath, rpfg::SaveMode::Full, from, "Rpfg serif - page 1",
                         "times", 1.0f, 0.0f, 0.0f) ||
            !expectText(styledPath, 0, "Rpfg serif - page 1", true) ||
            !expectText(styledPath, 0, from, false)) {
            return 1;
        }
        std::printf("PASS: replace with a chosen font and colour\n");
    }

    // --- erase a region (shapes / lines / images / text) --------------------
    {
        const std::string erasePath = (temp / "rpfg-erase.pdf").string();
        // Covers the heading but not the smaller line below it.
        if (!eraseAndSave(source, erasePath, 80.0f, 40.0f, 400.0f, 110.0f) ||
            !expectText(erasePath, 0, from, false) ||
            !expectText(erasePath, 0, "rasterised by MuPDF", true)) {
            return 1;
        }
        std::printf("PASS: erased a region\n");
    }

    // --- block edit: a multi-line selection, spacing preserved --------------
    {
        const std::string blockPath = (temp / "rpfg-block.pdf").string();
        if (!blockEditAndSave(source, blockPath, "First line\nSecond line") ||
            !expectText(blockPath, 0, "First line", true) ||
            !expectText(blockPath, 0, "Second line", true) ||
            !expectText(blockPath, 0, from, false)) {
            return 1;
        }
        std::printf("PASS: block edit (two lines, spacing kept)\n");
    }

    if (!embeddedFontId.empty()) {
        const std::string blockTtf = (temp / "rpfg-block-ttf.pdf").string();
        if (!blockEditAndSave(source, blockTtf, "Serif one\nSerif two", embeddedFontId) ||
            !expectText(blockTtf, 0, "Serif one", true) ||
            !expectText(blockTtf, 0, "Serif two", true)) {
            return 1;
        }
        std::printf("PASS: block edit with an embedded font\n");
    }

    std::printf("PASS\n");
    return 0;
}
