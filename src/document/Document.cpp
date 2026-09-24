#include "document/Document.h"

namespace rpfg {

Document::Document() = default;

std::shared_ptr<Page> Document::page(std::size_t index) const {
    if (index < pages_.size()) {
        return pages_[index];
    }
    return nullptr;
}

void Document::addPage(std::shared_ptr<Page> p) {
    if (p) {
        p->setIndex(static_cast<int>(pages_.size()));
        pages_.push_back(std::move(p));
        markDirty();
    }
}

void Document::insertPage(std::size_t index, std::shared_ptr<Page> p) {
    if (!p) return;
    if (index > pages_.size()) {
        index = pages_.size();
    }
    pages_.insert(pages_.begin() + index, std::move(p));
    for (std::size_t i = index; i < pages_.size(); ++i) {
        pages_[i]->setIndex(static_cast<int>(i));
    }
    markDirty();
}

bool Document::removePage(std::size_t index) {
    if (index < pages_.size()) {
        pages_.erase(pages_.begin() + index);
        for (std::size_t i = index; i < pages_.size(); ++i) {
            pages_[i]->setIndex(static_cast<int>(i));
        }
        markDirty();
        return true;
    }
    return false;
}

void Document::registerFont(DocumentFont font) {
    fonts_[font.id] = std::move(font);
}

const DocumentFont* Document::findFont(const FontId& id) const {
    auto it = fonts_.find(id);
    if (it != fonts_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::shared_ptr<Document> Document::clone() const {
    auto copy = std::make_shared<Document>();
    copy->title_ = title_;
    copy->author_ = author_;
    copy->sourcePath_ = sourcePath_;
    copy->fonts_ = fonts_;
    copy->dirty_ = dirty_;
    for (const auto& p : pages_) {
        if (p) {
            copy->pages_.push_back(p->clone());
        }
    }
    return copy;
}

}  // namespace rpfg
