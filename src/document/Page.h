#pragma once

#include <memory>
#include <string>
#include <vector>
#include "document/DocumentObject.h"
#include "document/Geometry.h"
#include "document/TextFrame.h"

namespace rpfg {

struct PageMargins {
    float left = 54.0f;    // 0.75 in
    float top = 54.0f;
    float right = 54.0f;
    float bottom = 54.0f;
};

class Page {
public:
    Page();
    Page(float width, float height, int index = 0);

    int index() const { return index_; }
    void setIndex(int idx) { index_ = idx; }

    float width() const { return width_; }
    void setWidth(float w) { width_ = w; }

    float height() const { return height_; }
    void setHeight(float h) { height_ = h; }

    Rect bounds() const { return Rect{0.0f, 0.0f, width_, height_}; }

    const PageMargins& margins() const { return margins_; }
    void setMargins(const PageMargins& m) { margins_ = m; }
    Rect contentBounds() const {
        return Rect{
            margins_.left,
            margins_.top,
            std::max(margins_.left, width_ - margins_.right),
            std::max(margins_.top, height_ - margins_.bottom)
        };
    }

    const std::vector<std::shared_ptr<DocumentObject>>& objects() const { return objects_; }
    std::vector<std::shared_ptr<DocumentObject>>& objects() { return objects_; }

    void addObject(std::shared_ptr<DocumentObject> obj);
    bool removeObject(const std::string& id);
    std::shared_ptr<DocumentObject> findObject(const std::string& id) const;

    std::vector<std::shared_ptr<DocumentObject>> objectsAt(float x, float y) const;
    std::vector<std::shared_ptr<TextFrame>> textFrames() const;

    std::shared_ptr<Page> clone() const;

private:
    int index_ = 0;
    float width_ = 612.0f;   // Standard US Letter default (points)
    float height_ = 792.0f;
    PageMargins margins_;
    std::vector<std::shared_ptr<DocumentObject>> objects_;
};

}  // namespace rpfg
