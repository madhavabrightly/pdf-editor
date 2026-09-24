#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace rpfg {

struct Point {
    float x = 0.0f;
    float y = 0.0f;

    bool operator==(const Point& other) const {
        return std::abs(x - other.x) < 1e-4f && std::abs(y - other.y) < 1e-4f;
    }
};

struct Rect {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;

    float width() const { return std::max(0.0f, x1 - x0); }
    float height() const { return std::max(0.0f, y1 - y0); }
    bool empty() const { return x1 <= x0 || y1 <= y0; }

    bool contains(float x, float y) const {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }

    bool contains(const Point& pt) const {
        return contains(pt.x, pt.y);
    }

    bool intersects(const Rect& other) const {
        return !(other.x0 > x1 || other.x1 < x0 || other.y0 > y1 || other.y1 < y0);
    }

    Rect united(const Rect& other) const {
        if (empty()) return other;
        if (other.empty()) return *this;
        return Rect{
            std::min(x0, other.x0),
            std::min(y0, other.y0),
            std::max(x1, other.x1),
            std::max(y1, other.y1)
        };
    }

    Rect intersected(const Rect& other) const {
        if (empty()) return *this;
        if (other.empty()) return other;
        return Rect{
            std::max(x0, other.x0),
            std::max(y0, other.y0),
            std::min(x1, other.x1),
            std::min(y1, other.y1)
        };
    }
};

struct Matrix {
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    float e = 0.0f;
    float f = 0.0f;

    static Matrix identity() {
        return Matrix{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    }

    bool isIdentity() const {
        return std::abs(a - 1.0f) < 1e-5f && std::abs(b) < 1e-5f &&
               std::abs(c) < 1e-5f && std::abs(d - 1.0f) < 1e-5f &&
               std::abs(e) < 1e-5f && std::abs(f) < 1e-5f;
    }

    static Matrix scale(float sx, float sy) {
        return Matrix{sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
    }

    static Matrix translate(float tx, float ty) {
        return Matrix{1.0f, 0.0f, 0.0f, 1.0f, tx, ty};
    }

    static Matrix rotate(float radians) {
        const float cosA = std::cos(radians);
        const float sinA = std::sin(radians);
        return Matrix{cosA, sinA, -sinA, cosA, 0.0f, 0.0f};
    }

    Point transform(const Point& p) const {
        return Point{
            p.x * a + p.y * c + e,
            p.x * b + p.y * d + f
        };
    }

    Rect transform(const Rect& r) const {
        if (r.empty()) return r;
        const Point p1 = transform(Point{r.x0, r.y0});
        const Point p2 = transform(Point{r.x1, r.y0});
        const Point p3 = transform(Point{r.x0, r.y1});
        const Point p4 = transform(Point{r.x1, r.y1});

        return Rect{
            std::min({p1.x, p2.x, p3.x, p4.x}),
            std::min({p1.y, p2.y, p3.y, p4.y}),
            std::max({p1.x, p2.x, p3.x, p4.x}),
            std::max({p1.y, p2.y, p3.y, p4.y})
        };
    }

    Matrix multiply(const Matrix& m) const {
        return Matrix{
            a * m.a + b * m.c,
            a * m.b + b * m.d,
            c * m.a + d * m.c,
            c * m.b + d * m.d,
            e * m.a + f * m.c + m.e,
            e * m.b + f * m.d + m.f
        };
    }

    Matrix inverse() const {
        const float det = a * d - b * c;
        if (std::abs(det) < 1e-6f) {
            return identity();
        }
        const float invDet = 1.0f / det;
        return Matrix{
            d * invDet,
            -b * invDet,
            -c * invDet,
            a * invDet,
            (c * f - d * e) * invDet,
            (b * e - a * f) * invDet
        };
    }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static Color black() { return Color{0.0f, 0.0f, 0.0f, 1.0f}; }
    static Color white() { return Color{1.0f, 1.0f, 1.0f, 1.0f}; }
    static Color transparent() { return Color{0.0f, 0.0f, 0.0f, 0.0f}; }

    bool operator==(const Color& other) const {
        return std::abs(r - other.r) < 1e-3f &&
               std::abs(g - other.g) < 1e-3f &&
               std::abs(b - other.b) < 1e-3f &&
               std::abs(a - other.a) < 1e-3f;
    }
};

enum class TextAlignment {
    Left,
    Center,
    Right,
    Justified
};

enum class PathVerb {
    MoveTo,
    LineTo,
    CubicTo,
    Close
};

struct PathCommand {
    PathVerb verb = PathVerb::MoveTo;
    Point p0;  // Target point for MoveTo/LineTo, or control1 for CubicTo
    Point p1;  // Control2 for CubicTo
    Point p2;  // Target point for CubicTo
};

}  // namespace rpfg
