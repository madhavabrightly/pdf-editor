#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "document/Geometry.h"

namespace rpfg {

enum class ObjectType {
    TextFrame,
    Image,
    Vector,
    Shape,
    Line,
    Annotation,
    Protected
};

class DocumentObject {
public:
    explicit DocumentObject(ObjectType type);
    virtual ~DocumentObject() = default;

    ObjectType type() const { return type_; }
    const std::string& id() const { return id_; }
    void setId(std::string id) { id_ = std::move(id); }

    int zIndex() const { return zIndex_; }
    void setZIndex(int z) { zIndex_ = z; }

    bool isVisible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }

    bool isLocked() const { return locked_; }
    void setLocked(bool l) { locked_ = l; }

    // Bounds in page coordinate space
    virtual Rect bounds() const = 0;
    virtual void setBounds(const Rect& bounds) = 0;

    // Local transformation matrix
    const Matrix& transform() const { return transform_; }
    void setTransform(const Matrix& m) { transform_ = m; }

    // Optional clipping rectangle
    const std::optional<Rect>& clipRect() const { return clipRect_; }
    void setClipRect(std::optional<Rect> r) { clipRect_ = std::move(r); }

    // Optional clipping path
    const std::vector<PathCommand>& clipPath() const { return clipPath_; }
    void setClipPath(std::vector<PathCommand> path) { clipPath_ = std::move(path); }
    bool hasClipPath() const { return !clipPath_.empty(); }

    // Deep clone
    virtual std::shared_ptr<DocumentObject> clone() const = 0;

private:
    ObjectType type_;
    std::string id_;
    int zIndex_ = 0;
    bool visible_ = true;
    bool locked_ = false;
    Matrix transform_ = Matrix::identity();
    std::optional<Rect> clipRect_;
    std::vector<PathCommand> clipPath_;
};

}  // namespace rpfg
