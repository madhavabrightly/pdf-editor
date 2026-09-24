#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include "document/Document.h"
#include "document/TextFrame.h"
#include "editor/CoordinateMapper.h"
#include "editor/EditorController.h"
#include "editor/TextHitTester.h"
#include "import/PdfDocumentImporter.h"

using namespace rpfg;

void testCoordinateMapper() {
    PageViewport vp;
    vp.zoom = 1.5f;
    vp.scrollX = 100.0f;
    vp.scrollY = 50.0f;
    vp.pageOffsetX = 200.0f;
    vp.pageOffsetY = 150.0f;
    vp.pageWidth = 612.0f;
    vp.pageHeight = 792.0f;
    vp.rotationDegrees = 0;

    Point pagePt{120.0f, 250.0f};
    Point viewportPt = CoordinateMapper::pageToViewport(pagePt, vp);
    Point roundtripPt = CoordinateMapper::viewportToPage(viewportPt, vp);

    assert(std::abs(roundtripPt.x - pagePt.x) < 0.001f);
    assert(std::abs(roundtripPt.y - pagePt.y) < 0.001f);

    // Test with rotation 90 degrees
    vp.rotationDegrees = 90;
    viewportPt = CoordinateMapper::pageToViewport(pagePt, vp);
    roundtripPt = CoordinateMapper::viewportToPage(viewportPt, vp);
    assert(std::abs(roundtripPt.x - pagePt.x) < 0.001f);
    assert(std::abs(roundtripPt.y - pagePt.y) < 0.001f);

    std::printf("PASS: CoordinateMapper zoom, scroll, rotation roundtrip\n");
}

void testHitTester(const std::string& pdfPath) {
    std::string error;
    PdfDocumentImporter importer;
    auto doc = importer.importDocument(pdfPath, error);
    assert(doc != nullptr);
    assert(!doc->pages().empty());

    const auto& page = *doc->pages()[0];

    // Find the first text frame to get its baseline
    Point samplePt{100.0f, 100.0f};
    for (const auto& obj : page.objects()) {
        if (obj && obj->type() == ObjectType::TextFrame) {
            auto frame = std::static_pointer_cast<TextFrame>(obj);
            if (!frame->paragraphs().empty() && !frame->paragraphs()[0].runs().empty()) {
                const auto& r = frame->paragraphs()[0].runs()[0];
                samplePt = r.originalGeometry().hasOriginalGeometry
                               ? r.originalGeometry().originalBaseline
                               : r.layoutBaseline();
                break;
            }
        }
    }

    DocumentPosition hit = TextHitTester::hitTest(page, samplePt, 0);
    assert(hit.isValid());

    CaretGeometry geom = TextHitTester::positionToGeometry(page, hit);
    assert(geom.height > 0.0f);
    assert(geom.top.y < geom.bottom.y);

    // Test selection rectangles
    DocumentPosition hit2 = hit;
    hit2.charOffset += 3;
    auto rects = TextHitTester::selectionToRectangles(page, hit, hit2);
    assert(!rects.empty());
    assert(rects[0].width() > 0.0f);
    assert(rects[0].height() > 0.0f);

    std::printf("PASS: TextHitTester hit testing, caret geometry, selection highlights\n");
}

void testEditorController() {
    auto doc = std::make_shared<Document>();
    auto page = std::make_shared<Page>(612.0f, 792.0f);

    auto frame = std::make_shared<TextFrame>(Rect{50.0f, 50.0f, 400.0f, 200.0f});
    Paragraph para;
    TextRun run("Hello World", TextStyle{});
    run.setLayoutBaseline(Point{50.0f, 70.0f});
    para.runs().push_back(run);
    frame->paragraphs().push_back(para);

    page->addObject(frame);
    doc->addPage(page);

    EditorController editor(doc);
    assert(editor.document() != nullptr);
    assert(editor.caretPosition().isValid());
    assert(editor.caretPosition().charOffset == 0);

    // 1. Move right
    editor.moveRight(false, false);
    assert(editor.caretPosition().charOffset == 1);

    // 2. Move by word
    editor.moveRight(false, true); // Move past "ello "
    assert(editor.caretPosition().charOffset > 1);

    // 3. Move to line start and end
    editor.moveToLineStart(false);
    assert(editor.caretPosition().charOffset == 0);
    editor.moveToLineEnd(false);
    assert(editor.caretPosition().charOffset == 11);

    // 4. Character insertion
    editor.insertText("!");
    assert(frame->paragraphs()[0].runs()[0].textUtf8() == "Hello World!");
    assert(editor.caretPosition().charOffset == 12);

    // 5. Undo and Redo
    assert(editor.canUndo());
    editor.undo();
    assert(editor.findActiveFrame()->paragraphs()[0].runs()[0].textUtf8() == "Hello World");
    assert(editor.canRedo());
    editor.redo();
    assert(editor.findActiveFrame()->paragraphs()[0].runs()[0].textUtf8() == "Hello World!");

    // 6. Backspace (deleteBackward)
    editor.deleteBackward();
    assert(editor.findActiveFrame()->paragraphs()[0].runs()[0].textUtf8() == "Hello World");

    // 7. Selection and replacement
    DocumentPosition p0 = editor.caretPosition();
    p0.charOffset = 6; // At 'W'
    DocumentPosition p1 = p0;
    p1.charOffset = 11; // After 'World'
    editor.setSelection(p0, p1);
    assert(editor.hasSelection());

    editor.insertText("Rpfg");
    assert(editor.findActiveFrame()->paragraphs()[0].runs()[0].textUtf8() == "Hello Rpfg");
    assert(!editor.hasSelection());

    // 8. Split paragraph (Enter)
    editor.splitParagraph();
    assert(editor.findActiveFrame()->paragraphs().size() == 2);
    assert(editor.findActiveFrame()->paragraphs()[0].runs()[0].textUtf8() == "Hello Rpfg");
    assert(editor.caretPosition().paragraphIndex == 1);

    // 9. Backspace across paragraphs (merge)
    editor.deleteBackward();
    assert(editor.findActiveFrame()->paragraphs().size() == 1);

    // 10. Render options
    RenderOptions opts = editor.getRenderOptions(1.0f);
    assert(opts.showCaret == true);

    std::printf("PASS: EditorController in-model editing, undo/redo, selection, and paragraph operations\n");
}

int main(int argc, char** argv) {
    std::string testPdf = "build/test.pdf";
    if (argc > 1) {
        testPdf = argv[1];
    }

    std::cout << "Running Editor & Interaction Tests..." << std::endl;
    testCoordinateMapper();
    testHitTester(testPdf);
    testEditorController();

    std::cout << "ALL EDITOR & INTERACTION TESTS PASSED!" << std::endl;
    return 0;
}
