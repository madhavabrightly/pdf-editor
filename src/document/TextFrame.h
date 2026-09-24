#pragma once

#include <memory>
#include <string>
#include <vector>
#include "document/DocumentObject.h"
#include "document/Paragraph.h"

namespace rpfg {

class TextFrame : public DocumentObject {
public:
    TextFrame();
    explicit TextFrame(const Rect& bounds);

    Rect bounds() const override { return bounds_; }
    void setBounds(const Rect& bounds) override { bounds_ = bounds; }

    const std::vector<Paragraph>& paragraphs() const { return paragraphs_; }
    std::vector<Paragraph>& paragraphs() { return paragraphs_; }

    void addParagraph(Paragraph p);
    void clear();

    void recalculateBounds() {
        if (paragraphs_.empty()) return;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const auto& p : paragraphs_) {
            for (const auto& r : p.runs()) {
                const Rect b = r.bounds();
                if (b.x0 < x0) x0 = b.x0;
                if (b.y0 < y0) y0 = b.y0;
                if (b.x1 > x1) x1 = b.x1;
                if (b.y1 > y1) y1 = b.y1;
            }
        }
        if (x0 <= x1 && y0 <= y1) {
            bounds_ = Rect{x0, y0, x1, y1};
        }
    }

    float paddingLeft() const { return paddingLeft_; }
    float paddingTop() const { return paddingTop_; }
    float paddingRight() const { return paddingRight_; }
    float paddingBottom() const { return paddingBottom_; }
    void setPadding(float l, float t, float r, float b) {
        paddingLeft_ = l;
        paddingTop_ = t;
        paddingRight_ = r;
        paddingBottom_ = b;
    }

    std::string textUtf8() const;
    std::shared_ptr<DocumentObject> clone() const override;

private:
    Rect bounds_;
    std::vector<Paragraph> paragraphs_;
    float paddingLeft_ = 0.0f;
    float paddingTop_ = 0.0f;
    float paddingRight_ = 0.0f;
    float paddingBottom_ = 0.0f;
};

}  // namespace rpfg
