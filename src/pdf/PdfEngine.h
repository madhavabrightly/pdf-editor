#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "document/TextModel.h"

namespace rpfg {

struct PageSize {
    float width = 0.0f;   // PDF points (1/72"), page box before any crop
    float height = 0.0f;
};

struct DocumentInfo {
    std::string path;
    std::string title;
    int pageCount = 0;
    std::vector<PageSize> pages;
};

struct RenderedPage {
    int page = 0;
    float scale = 1.0f;
    int pixelWidth = 0;
    int pixelHeight = 0;
    std::shared_ptr<const std::string> base64Png;
};

// The selectable text of one page, as a compact JSON run list:
//
//   {"runs":[{"x":,"y":,"s":,"w":,"a":,"asc":,"t":"..."}]}
//
// Coordinates are PDF points with (x, y) on the text baseline; `a` is the
// baseline angle in radians, `asc` the font ascender as a fraction of the font
// size. Zoom-independent, so it is computed once per page and cached by page -
// unlike rasters, which are keyed by page *and* zoom.
struct PageText {
    int page = 0;
    std::shared_ptr<const std::string> runs;
};

struct EngineStats {
    int workerThreads = 0;
    int queuedJobs = 0;
    std::uint64_t jobsSubmitted = 0;
    std::uint64_t jobsCoalesced = 0;
    std::uint64_t jobsCancelled = 0;
    std::uint64_t pagesRasterised = 0;
    std::uint64_t displayListHits = 0;
    std::uint64_t bitmapCacheHits = 0;
    std::size_t bitmapCacheBytes = 0;
    std::size_t bitmapCacheEntries = 0;
    std::uint64_t textLayersBuilt = 0;
    std::size_t textCacheEntries = 0;
    std::uint64_t editsApplied = 0;
};

// How a save should be written. Incremental appends just the changed objects to
// the existing file - milliseconds even for a huge document - and is what makes
// "save on every edit" affordable. Full rewrites the whole file, which is what
// Save As / Save a copy use (and the fallback when incremental is not possible).
enum class SaveMode {
    Incremental,
    Full,
    Copy,
};

// A font the editor can draw with. Standard fonts are the PDF base-14 (no
// embedding, every viewer already has them); embedded fonts are TrueType/OpenType
// files on disk that get subset-embedded into the document on first use.
struct FontInfo {
    std::string id;        // stable id exchanged over the bridge
    std::string label;     // shown in the picker
    bool embedded = false; // true => loaded from `path` and embedded
    std::string path;      // file to embed (embedded fonts only)
    std::string baseFont;  // base-14 name (standard fonts only)
};

// One text operation: either replace an existing run in place, or insert new
// text at a point. Geometry is in the same top-left-origin PDF-point space as
// PageText runs.
struct TextEdit {
    int page = 0;
    float x = 0.0f;          // baseline origin, points from the left
    float y = 0.0f;          // baseline origin, points from the top
    float width = 0.0f;      // original baseline advance (replace only)
    float size = 12.0f;      // font size
    float angle = 0.0f;      // baseline angle, radians
    float ascender = 0.8f;   // font ascender as a fraction of size (replace only)
    float r = 0.0f;          // fill colour, 0..1
    float g = 0.0f;
    float b = 0.0f;
    bool replace = true;     // false => insert, leaving existing content alone
    std::string font;        // FontInfo::id; empty means Helvetica
    std::string text;        // UTF-8 replacement / inserted text
};

// One line's original box, used to lay a re-edited paragraph back down where it
// was so its left margin and line spacing are preserved.
struct TextLineBox {
    float x = 0.0f;      // left edge of the line
    float y = 0.0f;      // baseline, points from the top
    float size = 0.0f;   // font size on that line
};

// A replacement spanning several runs - a paragraph or a column of text. The
// whole selection is erased with one redaction, then the text is redrawn line by
// line on the original baselines. Lines beyond the originals continue at the last
// line's leading.
struct TextBlockEdit {
    int page = 0;
    float x0 = 0.0f;   // erase box, page space
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float size = 12.0f;      // fallback size
    float spacing = 1.0f;    // line-spacing multiplier (1.0 = keep original)
    float r = 0.0f;          // fill colour
    float g = 0.0f;
    float b = 0.0f;
    std::string font;             // FontInfo::id; empty means Helvetica
    std::string text;             // '\n'-separated lines
    std::vector<TextLineBox> lines;
};

// Owns the MuPDF document and renders pages on a worker pool.
//
// Threading model
// ---------------
// The engine keeps one MuPDF context per worker thread (contexts are not
// thread-safe, but contexts cloned from a common parent share a locked
// resource store). Pages are turned into zoom-independent display lists once,
// under the document mutex, and those display lists are then rasterised
// concurrently - display lists are immutable and hold their own references to
// fonts and images, so a render never touches the document again.
class PdfEngine {
public:
    // Invoked for every freshly rasterised page. Called from whichever thread
    // produced it, so the caller is responsible for marshalling.
    using PageReadyCallback = std::function<void(std::shared_ptr<const RenderedPage>)>;

    // Invoked once per page when its text has been extracted.
    using PageTextCallback = std::function<void(std::shared_ptr<const PageText>)>;

    PdfEngine();
    ~PdfEngine();

    PdfEngine(const PdfEngine&) = delete;
    PdfEngine& operator=(const PdfEngine&) = delete;

    void setPageReadyCallback(PageReadyCallback callback);
    void setPageTextCallback(PageTextCallback callback);

    std::optional<DocumentInfo> openDocument(const std::string& utf8Path, std::string& error);
    void closeDocument();
    bool isOpen() const;

    // Replaces the glyphs of a run in place, or inserts new text. For a
    // replacement the old text is physically removed from the page content
    // stream (a text-only redaction, so images and vector art are untouched) and
    // the new text is appended in the same place. The page's display list,
    // rasters and text layer are dropped, so the caller must re-report the
    // viewport to see the result.
    bool applyTextEdit(const TextEdit& edit, std::string& error);

    // Edits several runs at once (a paragraph). The selection is erased and the
    // text is laid back down on the original line baselines, so left margin and
    // line spacing survive. Like applyTextEdit, page caches are invalidated.
    bool applyTextBlock(const TextBlockEdit& edit, std::string& error);

    // Removes everything drawn inside a page-space rectangle: vector paths
    // (shapes, lines, table rules), images and text. This is a redaction, so the
    // content is deleted from the stream, not painted over.
    bool eraseRegion(int page, float x0, float y0, float x1, float y1, std::string& error);

    // Fonts the editor can draw with: the PDF base-14 plus any TrueType/OpenType
    // files found in the fonts directory (see setFontsDirectory).
    std::vector<FontInfo> availableFonts() const;

    // The reconstructed native model of a page (characters -> words -> lines ->
    // paragraphs), available once that page's text has been extracted. Null if
    // the page has not been analysed yet.
    std::shared_ptr<const PageModel> pageModel(int page) const;

    // A directory scanned for extra .ttf/.otf/.ttc files. Call before
    // availableFonts(); typically "<exe dir>\\fonts".
    void setFontsDirectory(const std::string& utf8Dir);

    // Writes the document. Incremental appends to the existing file; Full
    // rewrites it (garbage-collected and compressed). An Incremental save that
    // is not possible (e.g. the file was repaired when opened) silently falls
    // back to Full.
    bool saveDocument(const std::string& utf8Path, SaveMode mode, std::string& error);

    bool hasUnsavedChanges() const;
    void markSaved();
    std::string currentPath() const;

    // Reports the pages that are (nearly) on screen and the current zoom.
    // Work for pages that drop out of the window is abandoned, so scrolling
    // fast never builds up a backlog of stale renders.
    void requestViewport(const std::vector<int>& pages, float scale);

    EngineStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rpfg
