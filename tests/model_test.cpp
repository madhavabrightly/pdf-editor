// Verifies the native document model: a real page is parsed into characters ->
// words -> lines -> paragraphs, and the structure is correct (a heading and a
// two-line paragraph must come out as two paragraphs, with the right word and
// line counts). This is the foundation the editor's layout and caret work on.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "pdf/PdfEngine.h"
#include "document/Document.h"
#include "document/Page.h"
#include "document/TextFrame.h"
#include "document/Paragraph.h"
#include "document/TextRun.h"
#include "document/LineObject.h"
#include "document/ShapeObject.h"
#include "document/VectorObject.h"
#include "document/ImageObject.h"
#include "document/ProtectedObject.h"

namespace {

std::mutex g_mutex;
std::condition_variable g_ready;
bool g_ready_flag = false;

void onText(std::shared_ptr<const rpfg::PageText> /*text*/) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_ready_flag = true;
    g_ready.notify_all();
}

bool analyse(const std::string& path, rpfg::PdfEngine& engine, int page) {
    engine.setPageTextCallback(onText);
    std::string error;
    if (!engine.openDocument(path, error).has_value()) {
        std::fprintf(stderr, "FAIL: could not open %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    engine.requestViewport({page}, 1.0f);

    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_ready.wait_for(lock, std::chrono::seconds(60), [] { return g_ready_flag; })) {
        std::fprintf(stderr, "FAIL: no text arrived\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rpfg_model_test <file.pdf>\n");
        return 2;
    }

    rpfg::PdfEngine engine;
    if (!analyse(argv[1], engine, 0)) {
        return 1;
    }

    const std::shared_ptr<const rpfg::PageModel> model = engine.pageModel(0);
    if (!model) {
        std::fprintf(stderr, "FAIL: page model was not built\n");
        return 1;
    }

    // The generator draws one heading and one smaller line, well apart.
    std::printf("glyphs=%zu paragraphs=%zu\n", model->glyphs.size(), model->paragraphs.size());
    for (std::size_t i = 0; i < model->paragraphs.size(); ++i) {
        const rpfg::StextParagraph& paragraph = model->paragraphs[i];
        std::printf("  paragraph %zu: lines=%zu size=%.1f leading=%.1f text=\"%s\"\n", i,
                    paragraph.lines.size(), paragraph.size, paragraph.leading,
                    paragraph.text().c_str());
    }

    if (model->glyphs.empty()) {
        std::fprintf(stderr, "FAIL: no glyphs in the model\n");
        return 1;
    }
    if (model->paragraphs.size() != 2) {
        std::fprintf(stderr, "FAIL: expected 2 paragraphs (heading + line), got %zu\n",
                     model->paragraphs.size());
        return 1;
    }

    const rpfg::StextParagraph& heading = model->paragraphs[0];
    if (heading.lines.size() != 1 || heading.lines.front().words.size() != 5) {
        std::fprintf(stderr, "FAIL: heading structure wrong (%zu lines, %zu words)\n",
                     heading.lines.size(), heading.lines.front().words.size());
        return 1;
    }
    if (heading.text() != "Rpfg test - page 1") {
        std::fprintf(stderr, "FAIL: heading text is \"%s\"\n", heading.text().c_str());
        return 1;
    }
    if (heading.size < 30.0f) {
        std::fprintf(stderr, "FAIL: heading size %.1f does not match the 36pt drawn\n",
                     heading.size);
        return 1;
    }

    const rpfg::StextParagraph& body = model->paragraphs[1];
    if (body.text().find("rasterised by MuPDF") == std::string::npos) {
        std::fprintf(stderr, "FAIL: body text is \"%s\"\n", body.text().c_str());
        return 1;
    }

    // A structural unit test for the grouping itself, independent of any PDF.
    {
        std::vector<rpfg::Glyph> glyphs;
        auto add = [&glyphs](const std::string& text, float x, float y, float size) {
            rpfg::Glyph glyph;
            glyph.text = text;
            glyph.x = x;
            glyph.y = y;
            glyph.size = size;
            glyph.advance = size * 0.5f;
            glyphs.push_back(glyph);
        };
        // "Hello world" on two lines, both 12pt, 14pt leading.
        const std::string line1 = "Hello world";
        float x = 50.0f;
        for (const char ch : line1) {
            add(std::string(1, ch == ' ' ? ' ' : ch), x, 100.0f, 12.0f);
            x += ch == ' ' ? 6.0f : 7.0f;
        }
        x = 50.0f;
        for (const char ch : std::string("second line")) {
            add(std::string(1, ch), x, 114.0f, 12.0f);
            x += ch == ' ' ? 6.0f : 7.0f;
        }
        const rpfg::PageModel grouped = rpfg::buildPageModel(glyphs);
        if (grouped.paragraphs.size() != 1 || grouped.paragraphs[0].lines.size() != 2 ||
            grouped.paragraphs[0].lines[0].words.size() != 2) {
            std::fprintf(stderr, "FAIL: synthetic grouping wrong (%zu paragraphs)\n",
                         grouped.paragraphs.size());
            return 1;
        }
        std::printf("synthetic: \"%s\" (leading %.1f)\n", grouped.paragraphs[0].text().c_str(),
                    grouped.paragraphs[0].leading);
        if (grouped.paragraphs[0].lines[0].text() != "Hello world") {
            std::fprintf(stderr, "FAIL: synthetic line 1 text is \"%s\"\n",
                         grouped.paragraphs[0].lines[0].text().c_str());
            return 1;
        }
    }

    // =========================================================================
    // SECTION 8 & DOCUMENT MODEL TESTS: Exact Spaces & Real Unicode Hierarchy
    // =========================================================================
    {
        // 1. TextRun with exact multiple spaces ("AI  Systems" has 2 spaces)
        const std::string textWithTwoSpaces = "AI  Systems";
        rpfg::TextRun run(textWithTwoSpaces);
        if (run.length() != 11) {
            std::fprintf(stderr, "FAIL: TextRun length %zu != 11\n", run.length());
            return 1;
        }
        if (run.text()[2] != U' ' || run.text()[3] != U' ') {
            std::fprintf(stderr, "FAIL: TextRun does not contain exact two spaces\n");
            return 1;
        }
        if (run.textUtf8() != "AI  Systems") {
            std::fprintf(stderr, "FAIL: TextRun UTF-8 is '%s'\n", run.textUtf8().c_str());
            return 1;
        }
        std::printf("PASS: Section 8 - TextRun preserves exact multiple spaces\n");

        // 2. Character editing: "AI Systems Architect" -> insert "Senior "
        rpfg::Paragraph p("AI Systems Architect");
        // offset 3 is after "AI "
        p.insertTextUtf8(3, "Senior ");
        if (p.textUtf8() != "AI Senior Systems Architect") {
            std::fprintf(stderr, "FAIL: insertion gave '%s'\n", p.textUtf8().c_str());
            return 1;
        }
        std::printf("PASS: Acceptance Test B - in-model character insertion: '%s'\n", p.textUtf8().c_str());

        // 3. Space editing: delete space -> "AISystems Architect"
        // in "AI Senior Systems Architect", delete space between "AI" and "Senior" (offset 2)
        p.deleteText(2, 1);
        if (p.textUtf8() != "AISenior Systems Architect") {
            std::fprintf(stderr, "FAIL: delete space gave '%s'\n", p.textUtf8().c_str());
            return 1;
        }

        // Reset to "AI Systems Architect"
        rpfg::Paragraph p2("AI Systems Architect");
        // Delete space at index 2 -> "AISystems Architect"
        p2.deleteText(2, 1);
        if (p2.textUtf8() != "AISystems Architect") {
            std::fprintf(stderr, "FAIL: delete space gave '%s'\n", p2.textUtf8().c_str());
            return 1;
        }
        // Insert two spaces back at index 2 -> "AI  Systems Architect"
        p2.insertTextUtf8(2, "  ");
        if (p2.textUtf8() != "AI  Systems Architect") {
            std::fprintf(stderr, "FAIL: insert two spaces gave '%s'\n", p2.textUtf8().c_str());
            return 1;
        }
        if (p2.textU32()[2] != U' ' || p2.textU32()[3] != U' ') {
            std::fprintf(stderr, "FAIL: p2 text does not have two spaces at index 2 and 3\n");
            return 1;
        }
        std::printf("PASS: Acceptance Test C - exact space deletion and multi-space insertion: '%s'\n", p2.textUtf8().c_str());

        // 4. Word replacement: "Python Engineer" -> replace "Engineer" with "Developer"
        rpfg::Paragraph p3("Python Engineer");
        // "Python " has length 7. "Engineer" is 8 chars.
        p3.deleteText(7, 8);
        p3.insertTextUtf8(7, "Developer");
        if (p3.textUtf8() != "Python Developer") {
            std::fprintf(stderr, "FAIL: word edit gave '%s'\n", p3.textUtf8().c_str());
            return 1;
        }
        std::printf("PASS: Acceptance Test D - word edit: '%s'\n", p3.textUtf8().c_str());

        // 5. Paragraph split & merge
        rpfg::Paragraph p4("First Line Text. Second Line Text.");
        rpfg::Paragraph pTail = p4.split(17); // splits before "Second"
        if (p4.textUtf8() != "First Line Text. " || pTail.textUtf8() != "Second Line Text.") {
            std::fprintf(stderr, "FAIL: split gave '%s' and '%s'\n", p4.textUtf8().c_str(), pTail.textUtf8().c_str());
            return 1;
        }
        p4.merge(std::move(pTail));
        if (p4.textUtf8() != "First Line Text. Second Line Text.") {
            std::fprintf(stderr, "FAIL: merge gave '%s'\n", p4.textUtf8().c_str());
            return 1;
        }
        std::printf("PASS: Paragraph split and merge\n");

        // 6. Complete Document hierarchy & Object Model (Sections 6, 24)
        rpfg::Document doc;
        auto page = std::make_shared<rpfg::Page>(612.0f, 792.0f, 0);

        // TextFrame
        auto frame = std::make_shared<rpfg::TextFrame>(rpfg::Rect{50.0f, 50.0f, 500.0f, 700.0f});
        frame->addParagraph(std::move(p4));
        page->addObject(frame);

        // LineObject
        auto lineObj = std::make_shared<rpfg::LineObject>(rpfg::Point{50.0f, 100.0f}, rpfg::Point{500.0f, 100.0f}, rpfg::Color::black(), 1.5f);
        page->addObject(lineObj);

        // ShapeObject
        auto rectObj = std::make_shared<rpfg::ShapeObject>(rpfg::ShapeType::Rectangle, rpfg::Rect{60.0f, 120.0f, 200.0f, 180.0f});
        page->addObject(rectObj);

        // VectorObject
        auto vecObj = std::make_shared<rpfg::VectorObject>();
        vecObj->appendCommand(rpfg::PathCommand{rpfg::PathVerb::MoveTo, rpfg::Point{10.0f, 10.0f}});
        vecObj->appendCommand(rpfg::PathCommand{rpfg::PathVerb::LineTo, rpfg::Point{20.0f, 30.0f}});
        vecObj->appendCommand(rpfg::PathCommand{rpfg::PathVerb::Close});
        page->addObject(vecObj);

        // ProtectedObject
        auto protObj = std::make_shared<rpfg::ProtectedObject>(rpfg::Rect{0.0f, 0.0f, 100.0f, 100.0f}, "unsupported shading pattern", 42);
        page->addObject(protObj);

        doc.addPage(page);

        if (doc.pageCount() != 1) {
            std::fprintf(stderr, "FAIL: doc pageCount is %zu\n", doc.pageCount());
            return 1;
        }
        if (doc.page(0)->objects().size() != 5) {
            std::fprintf(stderr, "FAIL: page object count is %zu != 5\n", doc.page(0)->objects().size());
            return 1;
        }

        // Test deep clone
        auto clonedDoc = doc.clone();
        if (clonedDoc->pageCount() != 1 || clonedDoc->page(0)->objects().size() != 5) {
            std::fprintf(stderr, "FAIL: cloned doc objects wrong\n");
            return 1;
        }
        std::printf("PASS: Document hierarchy, polymorphism, and cloning (TextFrame, Line, Shape, Vector, Protected)\n");
    }

    std::printf("ALL MODEL TESTS PASSED\n");
    return 0;
}
