#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "document/Document.h"
#include "editor/TextHitTester.h"
#include "render/DocumentRenderer.h"

namespace rpfg {

class EditorController {
public:
    EditorController();
    explicit EditorController(std::shared_ptr<Document> doc);
    ~EditorController() = default;

    // Document binding
    void setDocument(std::shared_ptr<Document> doc);
    std::shared_ptr<Document> document() const { return document_; }
    int activePageIndex() const { return activePageIndex_; }
    void setActivePageIndex(int index);

    // Caret and selection state
    const DocumentPosition& caretPosition() const { return caretPos_; }
    void setCaretPosition(const DocumentPosition& pos);

    bool hasSelection() const;
    const std::optional<DocumentPosition>& selectionAnchor() const { return selAnchor_; }
    void setSelection(DocumentPosition anchor, DocumentPosition focus);
    void clearSelection();
    void selectAll();

    bool caretVisible() const { return caretVisible_; }
    void setCaretVisible(bool visible) { caretVisible_ = visible; }
    void toggleCaretBlink() { caretVisible_ = !caretVisible_; }

    // Hit-testing from viewport click
    void handlePointerDown(Point pagePt, bool shiftKey = false);
    void handlePointerMove(Point pagePt);

    // Navigation
    void moveLeft(bool extendSelection = false, bool byWord = false);
    void moveRight(bool extendSelection = false, bool byWord = false);
    void moveUp(bool extendSelection = false);
    void moveDown(bool extendSelection = false);
    void moveToLineStart(bool extendSelection = false);
    void moveToLineEnd(bool extendSelection = false);
    void moveToDocumentStart(bool extendSelection = false);
    void moveToDocumentEnd(bool extendSelection = false);

    // In-model Text Editing
    bool insertText(const std::string& textUtf8);
    bool deleteBackward();
    bool deleteForward();
    bool splitParagraph();

    // Undo / Redo
    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    bool undo();
    bool redo();

    // Render configuration for live display of document, caret, and selection
    RenderOptions getRenderOptions(float scale = 1.0f) const;

    // Active frame lookup
    std::shared_ptr<TextFrame> findActiveFrame() const;

private:
    void pushUndoState();
    void deleteSelectedRange();

    std::shared_ptr<Document> document_;
    int activePageIndex_ = 0;
    DocumentPosition caretPos_;
    std::optional<DocumentPosition> selAnchor_;
    bool caretVisible_ = true;

    std::vector<std::shared_ptr<Document>> undoStack_;
    std::vector<std::shared_ptr<Document>> redoStack_;
    static constexpr std::size_t kMaxUndoHistory = 50;
};

}  // namespace rpfg
