#include "document/Page.h"

#include <algorithm>

namespace rpfg {

Page::Page() = default;

Page::Page(float width, float height, int index)
    : index_(index), width_(width), height_(height) {}

void Page::addObject(std::shared_ptr<DocumentObject> obj) {
    if (obj) {
        objects_.push_back(std::move(obj));
    }
}

bool Page::removeObject(const std::string& id) {
    auto it = std::remove_if(objects_.begin(), objects_.end(),
                             [&](const std::shared_ptr<DocumentObject>& obj) {
                                 return obj && obj->id() == id;
                             });
    if (it != objects_.end()) {
        objects_.erase(it, objects_.end());
        return true;
    }
    return false;
}

std::shared_ptr<DocumentObject> Page::findObject(const std::string& id) const {
    for (const auto& obj : objects_) {
        if (obj && obj->id() == id) {
            return obj;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<DocumentObject>> Page::objectsAt(float x, float y) const {
    std::vector<std::shared_ptr<DocumentObject>> hits;
    for (auto it = objects_.rbegin(); it != objects_.rend(); ++it) {
        if (*it && (*it)->isVisible() && (*it)->bounds().contains(x, y)) {
            hits.push_back(*it);
        }
    }
    return hits;
}

std::vector<std::shared_ptr<TextFrame>> Page::textFrames() const {
    std::vector<std::shared_ptr<TextFrame>> frames;
    for (const auto& obj : objects_) {
        if (obj && obj->type() == ObjectType::TextFrame) {
            frames.push_back(std::static_pointer_cast<TextFrame>(obj));
        }
    }
    return frames;
}

std::shared_ptr<Page> Page::clone() const {
    auto copy = std::make_shared<Page>(width_, height_, index_);
    copy->setMargins(margins_);
    for (const auto& obj : objects_) {
        if (obj) {
            copy->addObject(obj->clone());
        }
    }
    return copy;
}

}  // namespace rpfg
