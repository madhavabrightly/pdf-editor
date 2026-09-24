#include "editor/EditorController.h"

#include <algorithm>
#include <cctype>

namespace rpfg {

EditorController::EditorController() = default;

EditorController::EditorController(std::shared_ptr<Document> doc) {
    setDocument(std::move(doc));
}

void EditorController::setDocument(std::shared_ptr<Document> doc) {
    document_ = std::move(doc);
    activePageIndex_ = 0;
    caretPos_ = DocumentPosition{};
    selAnchor_.reset();
    undoStack_.clear();
    redoStack_.clear();

    if (document_ && !document_->pages().empty()) {
        const auto& page = document_->pages()[0];
        if (page) {
            for (const auto& obj : page->objects()) {
                if (obj && obj->type() == ObjectType::TextFrame) {
                    auto frame = std::static_pointer_cast<TextFrame>(obj);
                    caretPos_.pageIndex = 0;
                    caretPos_.frameId = frame->id();
                    caretPos_.paragraphIndex = 0;
                    caretPos_.runIndex = 0;
                    caretPos_.charOffset = 0;
                    break;
                }
            }
        }
    }
}

void EditorController::setActivePageIndex(int index) {
    if (!document_ || index < 0 || static_cast<std::size_t>(index) >= document_->pages().size()) {
        return;
    }
    activePageIndex_ = index;
    clearSelection();
    caretPos_.pageIndex = index;
}

void EditorController::setCaretPosition(const DocumentPosition& pos) {
    caretPos_ = pos;
    selAnchor_.reset();
    caretVisible_ = true;
}

bool EditorController::hasSelection() const {
    if (!selAnchor_.has_value()) return false;
    return !(*selAnchor_ == caretPos_);
}

void EditorController::setSelection(DocumentPosition anchor, DocumentPosition focus) {
    selAnchor_ = anchor;
    caretPos_ = focus;
    caretVisible_ = true;
}

void EditorController::clearSelection() {
    selAnchor_.reset();
}

void EditorController::selectAll() {
    if (!document_ || activePageIndex_ < 0 || static_cast<std::size_t>(activePageIndex_) >= document_->pages().size()) {
        return;
    }
    const auto& page = document_->pages()[activePageIndex_];
    if (!page) return;

    for (const auto& obj : page->objects()) {
        if (!obj || obj->type() != ObjectType::TextFrame) continue;
        auto frame = std::static_pointer_cast<TextFrame>(obj);
        if (frame->paragraphs().empty()) continue;

        DocumentPosition start;
        start.pageIndex = activePageIndex_;
        start.frameId = frame->id();
        start.paragraphIndex = 0;
        start.runIndex = 0;
        start.charOffset = 0;

        const auto& lastPara = frame->paragraphs().back();
        DocumentPosition end;
        end.pageIndex = activePageIndex_;
        end.frameId = frame->id();
        end.paragraphIndex = frame->paragraphs().size() - 1;
        if (!lastPara.runs().empty()) {
            end.runIndex = lastPara.runs().size() - 1;
            end.charOffset = lastPara.runs().back().textUtf8().size();
        } else {
            end.runIndex = 0;
            end.charOffset = 0;
        }

        setSelection(start, end);
        break;
    }
}

void EditorController::handlePointerDown(Point pagePt, bool shiftKey) {
    if (!document_ || activePageIndex_ < 0 || static_cast<std::size_t>(activePageIndex_) >= document_->pages().size()) {
        return;
    }
    const auto& page = document_->pages()[activePageIndex_];
    if (!page) return;

    DocumentPosition hit = TextHitTester::hitTest(*page, pagePt, activePageIndex_);
    if (!hit.isValid()) return;

    if (shiftKey) {
        if (!selAnchor_.has_value()) {
            selAnchor_ = caretPos_;
        }
        caretPos_ = hit;
    } else {
        selAnchor_ = hit;
        caretPos_ = hit;
    }
    caretVisible_ = true;
}

void EditorController::handlePointerMove(Point pagePt) {
    if (!document_ || activePageIndex_ < 0 || static_cast<std::size_t>(activePageIndex_) >= document_->pages().size()) {
        return;
    }
    const auto& page = document_->pages()[activePageIndex_];
    if (!page) return;

    DocumentPosition hit = TextHitTester::hitTest(*page, pagePt, activePageIndex_);
    if (hit.isValid()) {
        caretPos_ = hit;
        caretVisible_ = true;
    }
}

std::shared_ptr<TextFrame> EditorController::findActiveFrame() const {
    if (!document_ || activePageIndex_ < 0 || static_cast<std::size_t>(activePageIndex_) >= document_->pages().size()) {
        return nullptr;
    }
    const auto& page = document_->pages()[activePageIndex_];
    if (!page) return nullptr;

    for (const auto& obj : page->objects()) {
        if (obj && obj->type() == ObjectType::TextFrame && obj->id() == caretPos_.frameId) {
            return std::static_pointer_cast<TextFrame>(obj);
        }
    }
    return nullptr;
}

void EditorController::moveLeft(bool extendSelection, bool byWord) {
    if (extendSelection && !selAnchor_.has_value()) {
        selAnchor_ = caretPos_;
    } else if (!extendSelection) {
        selAnchor_.reset();
    }

    auto frame = findActiveFrame();
    if (!frame) return;

    if (caretPos_.paragraphIndex >= frame->paragraphs().size()) return;
    const auto& para = frame->paragraphs()[caretPos_.paragraphIndex];
    if (caretPos_.runIndex >= para.runs().size()) return;
    const auto& run = para.runs()[caretPos_.runIndex];

    if (byWord) {
        if (caretPos_.charOffset > 0) {
            const std::string& text = run.textUtf8();
            std::size_t pos = caretPos_.charOffset;
            while (pos > 0 && std::isspace(static_cast<unsigned char>(text[pos - 1]))) --pos;
            while (pos > 0 && !std::isspace(static_cast<unsigned char>(text[pos - 1]))) --pos;
            caretPos_.charOffset = pos;
        } else if (caretPos_.runIndex > 0) {
            --caretPos_.runIndex;
            caretPos_.charOffset = para.runs()[caretPos_.runIndex].textUtf8().size();
        }
    } else {
        if (caretPos_.charOffset > 0) {
            --caretPos_.charOffset;
        } else if (caretPos_.runIndex > 0) {
            --caretPos_.runIndex;
            caretPos_.charOffset = para.runs()[caretPos_.runIndex].textUtf8().size();
        } else if (caretPos_.paragraphIndex > 0) {
            --caretPos_.paragraphIndex;
            const auto& prevPara = frame->paragraphs()[caretPos_.paragraphIndex];
            if (!prevPara.runs().empty()) {
                caretPos_.runIndex = prevPara.runs().size() - 1;
                caretPos_.charOffset = prevPara.runs().back().textUtf8().size();
            } else {
                caretPos_.runIndex = 0;
                caretPos_.charOffset = 0;
            }
        }
    }
    caretVisible_ = true;
}

void EditorController::moveRight(bool extendSelection, bool byWord) {
    if (extendSelection && !selAnchor_.has_value()) {
        selAnchor_ = caretPos_;
    } else if (!extendSelection) {
        selAnchor_.reset();
    }

    auto frame = findActiveFrame();
    if (!frame) return;

    if (caretPos_.paragraphIndex >= frame->paragraphs().size()) return;
    const auto& para = frame->paragraphs()[caretPos_.paragraphIndex];
    if (caretPos_.runIndex >= para.runs().size()) return;
    const auto& run = para.runs()[caretPos_.runIndex];

    if (byWord) {
        const std::string& text = run.textUtf8();
        if (caretPos_.charOffset < text.size()) {
            std::size_t pos = caretPos_.charOffset;
            while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            caretPos_.charOffset = pos;
        } else if (caretPos_.runIndex + 1 < para.runs().size()) {
            ++caretPos_.runIndex;
            caretPos_.charOffset = 0;
        }
    } else {
        if (caretPos_.charOffset < run.textUtf8().size()) {
            ++caretPos_.charOffset;
        } else if (caretPos_.runIndex + 1 < para.runs().size()) {
            ++caretPos_.runIndex;
            caretPos_.charOffset = 0;
        } else if (caretPos_.paragraphIndex + 1 < frame->paragraphs().size()) {
            ++caretPos_.paragraphIndex;
            caretPos_.runIndex = 0;
            caretPos_.charOffset = 0;
        }
    }
    caretVisible_ = true;
}

void EditorController::moveUp(bool extendSelection) {
    if (extendSelection && !selAnchor_.has_value()) {
        selAnchor_ = caretPos_;
    } else if (!extendSelection) {
        selAnchor_.reset();
    }

    auto frame = findActiveFrame();
    if (!frame) return;

    if (caretPos_.paragraphIndex > 0) {
        --caretPos_.paragraphIndex;
        const auto& para = frame->paragraphs()[caretPos_.paragraphIndex];
        if (!para.runs().empty()) {
            caretPos_.runIndex = std::min(caretPos_.runIndex, para.runs().size() - 1);
            caretPos_.charOffset = std::min(caretPos_.charOffset, para.runs()[caretPos_.runIndex].textUtf8().size());
        } else {
            caretPos_.runIndex = 0;
            caretPos_.charOffset = 0;
        }
    } else {
        caretPos_.charOffset = 0;
    }
    caretVisible_ = true;
}

void EditorController::moveDown(bool extendSelection) {
    if (extendSelection && !selAnchor_.has_value()) {
        selAnchor_ = caretPos_;
    } else if (!extendSelection) {
        selAnchor_.reset();
    }

    auto frame = findActiveFrame();
    if (!frame) return;

    if (caretPos_.paragraphIndex + 1 < frame->paragraphs().size()) {
        ++caretPos_.paragraphIndex;
        const auto& para = frame->paragraphs()[caretPos_.paragraphIndex];
        if (!para.runs().empty()) {
            caretPos_.runIndex = std::min(caretPos_.runIndex, para.runs().size() - 1);
            caretPos_.charOffset = std::min(caretPos_.charOffset, para.runs()[caretPos_.runIndex].textUtf8().size());
        } else {
            caretPos_.runIndex = 0;
            caretPos_.charOffset = 0;
        }
    } else {
        const auto& para = frame->paragraphs()[caretPos_.paragraphIndex];
        if (!para.runs().empty()) {
            caretPos_.runIndex = para.runs().size() - 1;
            caretPos_.charOffset = para.runs().back().textUtf8().size();
        }
    }
    caretVisible_ = true;
}

void EditorController::moveToLineStart(bool extendSelection) {
    if (extendSelection && !selAnchor_.has_value()) {
        selAnchor_ = caretPos_;
    } else if (!extendSelection) {
        selAnchor_.reset();
    }
    caretPos_.charOffset = 0;
    caretVisible_ = true;
}

void EditorController::moveToLineEnd(bool extendSelection) {
    if (extendSelection && !selAnchor_.has_value()) {
        selAnchor_ = caretPos_;
    } else if (!extendSelection) {
        selAnchor_.reset();
    }
    auto frame = findActiveFrame();
    if (frame && caretPos_.paragraphIndex < frame->paragraphs().size()) {
        const auto& para = frame->paragraphs()[caretPos_.paragraphIndex];
        if (caretPos_.runIndex < para.runs().size()) {
            caretPos_.charOffset = para.runs()[caretPos_.runIndex].textUtf8().size();
        }
    }
    caretVisible_ = true;
}

void EditorController::moveToDocumentStart(bool extendSelection) {
    if (extendSelection && !selAnchor_.has_value()) {
        selAnchor_ = caretPos_;
    } else if (!extendSelection) {
        selAnchor_.reset();
    }
    caretPos_.paragraphIndex = 0;
    caretPos_.runIndex = 0;
    caretPos_.charOffset = 0;
    caretVisible_ = true;
}

void EditorController::moveToDocumentEnd(bool extendSelection) {
    if (extendSelection && !selAnchor_.has_value()) {
        selAnchor_ = caretPos_;
    } else if (!extendSelection) {
        selAnchor_.reset();
    }
    auto frame = findActiveFrame();
    if (frame && !frame->paragraphs().empty()) {
        caretPos_.paragraphIndex = frame->paragraphs().size() - 1;
        const auto& para = frame->paragraphs().back();
        if (!para.runs().empty()) {
            caretPos_.runIndex = para.runs().size() - 1;
            caretPos_.charOffset = para.runs().back().textUtf8().size();
        }
    }
    caretVisible_ = true;
}

void EditorController::pushUndoState() {
    if (!document_) return;
    undoStack_.push_back(document_->clone());
    if (undoStack_.size() > kMaxUndoHistory) {
        undoStack_.erase(undoStack_.begin());
    }
    redoStack_.clear();
}

bool EditorController::undo() {
    if (undoStack_.empty()) return false;
    redoStack_.push_back(document_->clone());
    document_ = undoStack_.back();
    undoStack_.pop_back();
    clearSelection();
    return true;
}

bool EditorController::redo() {
    if (redoStack_.empty()) return false;
    undoStack_.push_back(document_->clone());
    document_ = redoStack_.back();
    redoStack_.pop_back();
    clearSelection();
    return true;
}

void EditorController::deleteSelectedRange() {
    if (!hasSelection()) return;

    DocumentPosition p0 = *selAnchor_;
    DocumentPosition p1 = caretPos_;
    bool swapped = false;
    if (p0.paragraphIndex > p1.paragraphIndex ||
        (p0.paragraphIndex == p1.paragraphIndex && p0.runIndex > p1.runIndex) ||
        (p0.paragraphIndex == p1.paragraphIndex && p0.runIndex == p1.runIndex && p0.charOffset > p1.charOffset)) {
        std::swap(p0, p1);
        swapped = true;
    }

    auto frame = findActiveFrame();
    if (!frame) return;

    if (p0.paragraphIndex == p1.paragraphIndex && p0.runIndex == p1.runIndex) {
        if (p0.paragraphIndex < frame->paragraphs().size()) {
            auto& para = frame->paragraphs()[p0.paragraphIndex];
            if (p0.runIndex < para.runs().size()) {
                auto& run = para.runs()[p0.runIndex];
                if (p1.charOffset > p0.charOffset) {
                    run.erase(p0.charOffset, p1.charOffset - p0.charOffset);
                }
            }
        }
    }

    caretPos_ = p0;
    selAnchor_.reset();
}

bool EditorController::insertText(const std::string& textUtf8) {
    if (textUtf8.empty()) return false;

    pushUndoState();
    if (hasSelection()) {
        deleteSelectedRange();
    }

    auto frame = findActiveFrame();
    if (!frame) return false;

    if (frame->paragraphs().empty()) {
        frame->paragraphs().emplace_back();
    }
    if (caretPos_.paragraphIndex >= frame->paragraphs().size()) {
        caretPos_.paragraphIndex = frame->paragraphs().size() - 1;
    }
    auto& para = frame->paragraphs()[caretPos_.paragraphIndex];
    if (para.runs().empty()) {
        para.runs().emplace_back();
    }
    if (caretPos_.runIndex >= para.runs().size()) {
        caretPos_.runIndex = para.runs().size() - 1;
    }
    auto& run = para.runs()[caretPos_.runIndex];

    const std::size_t offset = std::min(caretPos_.charOffset, run.textUtf8().size());
    run.insertText(offset, textUtf8);
    caretPos_.charOffset = offset + textUtf8.size();

    // Recalculate bounds
    frame->recalculateBounds();
    return true;
}

bool EditorController::deleteBackward() {
    pushUndoState();
    if (hasSelection()) {
        deleteSelectedRange();
        return true;
    }

    auto frame = findActiveFrame();
    if (!frame) return false;

    if (caretPos_.paragraphIndex >= frame->paragraphs().size()) return false;
    auto& para = frame->paragraphs()[caretPos_.paragraphIndex];
    if (caretPos_.runIndex >= para.runs().size()) return false;
    auto& run = para.runs()[caretPos_.runIndex];

    if (caretPos_.charOffset > 0) {
        run.erase(caretPos_.charOffset - 1, 1);
        --caretPos_.charOffset;
        frame->recalculateBounds();
        return true;
    } else if (caretPos_.runIndex > 0) {
        --caretPos_.runIndex;
        auto& prevRun = para.runs()[caretPos_.runIndex];
        caretPos_.charOffset = prevRun.textUtf8().size();
        if (caretPos_.charOffset > 0) {
            prevRun.erase(caretPos_.charOffset - 1, 1);
            --caretPos_.charOffset;
        }
        frame->recalculateBounds();
        return true;
    } else if (caretPos_.paragraphIndex > 0) {
        // Merge current paragraph into previous paragraph
        auto& prevPara = frame->paragraphs()[caretPos_.paragraphIndex - 1];
        caretPos_.runIndex = prevPara.runs().empty() ? 0 : prevPara.runs().size() - 1;
        caretPos_.charOffset = prevPara.runs().empty() ? 0 : prevPara.runs().back().textUtf8().size();

        for (auto& r : para.runs()) {
            prevPara.runs().push_back(std::move(r));
        }
        frame->paragraphs().erase(frame->paragraphs().begin() + caretPos_.paragraphIndex);
        --caretPos_.paragraphIndex;
        frame->recalculateBounds();
        return true;
    }

    return false;
}

bool EditorController::deleteForward() {
    pushUndoState();
    if (hasSelection()) {
        deleteSelectedRange();
        return true;
    }

    auto frame = findActiveFrame();
    if (!frame) return false;

    if (caretPos_.paragraphIndex >= frame->paragraphs().size()) return false;
    auto& para = frame->paragraphs()[caretPos_.paragraphIndex];
    if (caretPos_.runIndex >= para.runs().size()) return false;
    auto& run = para.runs()[caretPos_.runIndex];

    if (caretPos_.charOffset < run.textUtf8().size()) {
        run.erase(caretPos_.charOffset, 1);
        frame->recalculateBounds();
        return true;
    } else if (caretPos_.runIndex + 1 < para.runs().size()) {
        auto& nextRun = para.runs()[caretPos_.runIndex + 1];
        if (!nextRun.textUtf8().empty()) {
            nextRun.erase(0, 1);
        }
        frame->recalculateBounds();
        return true;
    } else if (caretPos_.paragraphIndex + 1 < frame->paragraphs().size()) {
        // Merge next paragraph into current
        auto& nextPara = frame->paragraphs()[caretPos_.paragraphIndex + 1];
        for (auto& r : nextPara.runs()) {
            para.runs().push_back(std::move(r));
        }
        frame->paragraphs().erase(frame->paragraphs().begin() + caretPos_.paragraphIndex + 1);
        frame->recalculateBounds();
        return true;
    }

    return false;
}

bool EditorController::splitParagraph() {
    pushUndoState();
    if (hasSelection()) {
        deleteSelectedRange();
    }

    auto frame = findActiveFrame();
    if (!frame) return false;

    if (caretPos_.paragraphIndex >= frame->paragraphs().size()) return false;
    auto& para = frame->paragraphs()[caretPos_.paragraphIndex];

    Paragraph newPara;
    if (caretPos_.runIndex < para.runs().size()) {
        auto& currentRun = para.runs()[caretPos_.runIndex];
        const std::string text = currentRun.textUtf8();

        if (caretPos_.charOffset < text.size()) {
            TextRun splitRun = currentRun;
            splitRun.setTextUtf8(text.substr(caretPos_.charOffset));
            currentRun.setTextUtf8(text.substr(0, caretPos_.charOffset));
            newPara.runs().push_back(std::move(splitRun));
        }

        for (std::size_t i = caretPos_.runIndex + 1; i < para.runs().size(); ++i) {
            newPara.runs().push_back(std::move(para.runs()[i]));
        }
        para.runs().erase(para.runs().begin() + caretPos_.runIndex + 1, para.runs().end());
    }

    frame->paragraphs().insert(frame->paragraphs().begin() + caretPos_.paragraphIndex + 1, std::move(newPara));
    ++caretPos_.paragraphIndex;
    caretPos_.runIndex = 0;
    caretPos_.charOffset = 0;
    frame->recalculateBounds();
    return true;
}

RenderOptions EditorController::getRenderOptions(float scale) const {
    RenderOptions opts;
    opts.scale = scale;
    opts.showCaret = caretVisible_;

    if (!document_ || activePageIndex_ < 0 || static_cast<std::size_t>(activePageIndex_) >= document_->pages().size()) {
        return opts;
    }
    const auto& page = document_->pages()[activePageIndex_];
    if (!page) return opts;

    if (caretPos_.isValid()) {
        CaretGeometry geom = TextHitTester::positionToGeometry(*page, caretPos_);
        opts.caretPosition = geom.position;
        opts.caretHeight = geom.height;
    }

    if (hasSelection() && selAnchor_.has_value()) {
        opts.selectionRectangles = TextHitTester::selectionToRectangles(*page, *selAnchor_, caretPos_);
    }

    return opts;
}

}  // namespace rpfg
