#include "document/TextFrame.h"

namespace rpfg {

TextFrame::TextFrame()
    : DocumentObject(ObjectType::TextFrame) {}

TextFrame::TextFrame(const Rect& bounds)
    : DocumentObject(ObjectType::TextFrame), bounds_(bounds) {}

void TextFrame::addParagraph(Paragraph p) {
    paragraphs_.push_back(std::move(p));
}

void TextFrame::clear() {
    paragraphs_.clear();
}

std::string TextFrame::textUtf8() const {
    std::string out;
    for (std::size_t i = 0; i < paragraphs_.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out += paragraphs_[i].textUtf8();
    }
    return out;
}

std::shared_ptr<DocumentObject> TextFrame::clone() const {
    auto copy = std::make_shared<TextFrame>(bounds_);
    copy->setId(id());
    copy->setZIndex(zIndex());
    copy->setVisible(isVisible());
    copy->setLocked(isLocked());
    copy->setTransform(transform());
    copy->setClipRect(clipRect());
    copy->setClipPath(clipPath());
    copy->paragraphs_ = paragraphs_;
    copy->paddingLeft_ = paddingLeft_;
    copy->paddingTop_ = paddingTop_;
    copy->paddingRight_ = paddingRight_;
    copy->paddingBottom_ = paddingBottom_;
    return copy;
}

}  // namespace rpfg
