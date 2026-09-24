#include "editor/TextHitTester.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rpfg {
namespace {

float getCharAdvance(const TextRun& run) {
    if (run.textUtf8().empty()) {
        const float fs = run.style().fontSize > 0.0f ? run.style().fontSize : 12.0f;
        return fs * 0.5f;
    }
    const float runWidth = run.bounds().width();
    if (runWidth > 0.0f) {
        return runWidth / static_cast<float>(run.textUtf8().size());
    }
    const float fs = run.style().fontSize > 0.0f ? run.style().fontSize : 12.0f;
    return fs * 0.5f;
}

Point getRunBaseline(const TextRun& run) {
    if (run.originalGeometry().hasOriginalGeometry) {
        return run.originalGeometry().originalBaseline;
    }
    return run.layoutBaseline();
}

}  // namespace

DocumentPosition TextHitTester::hitTest(const Page& page, Point pagePt, int pageIndex) {
    DocumentPosition bestPos;
    float minDistanceSq = std::numeric_limits<float>::max();

    for (const auto& obj : page.objects()) {
        if (!obj || obj->type() != ObjectType::TextFrame) continue;
        auto frame = std::static_pointer_cast<TextFrame>(obj);

        const auto& paras = frame->paragraphs();
        for (std::size_t pi = 0; pi < paras.size(); ++pi) {
            const auto& para = paras[pi];
            const auto& runs = para.runs();

            for (std::size_t ri = 0; ri < runs.size(); ++ri) {
                const auto& run = runs[ri];
                const Point base = getRunBaseline(run);
                const float charAdv = getCharAdvance(run);
                const float fontSize = run.style().fontSize > 0.0f ? run.style().fontSize : 12.0f;
                const float topY = base.y - fontSize * 0.8f;
                const float bottomY = base.y + fontSize * 0.2f;

                // Vertical distance to this run's line
                float dy = 0.0f;
                if (pagePt.y < topY) {
                    dy = topY - pagePt.y;
                } else if (pagePt.y > bottomY) {
                    dy = pagePt.y - bottomY;
                }

                const std::size_t numChars = run.textUtf8().size();
                for (std::size_t ci = 0; ci <= numChars; ++ci) {
                    const float cx = base.x + static_cast<float>(ci) * charAdv;
                    const float dx = pagePt.x - cx;
                    // Weight vertical distance higher than horizontal to stay on line
                    const float distSq = dx * dx + (dy * 2.5f) * (dy * 2.5f);

                    if (distSq < minDistanceSq) {
                        minDistanceSq = distSq;
                        bestPos.pageIndex = pageIndex;
                        bestPos.frameId = frame->id();
                        bestPos.paragraphIndex = pi;
                        bestPos.runIndex = ri;
                        bestPos.charOffset = ci;
                    }
                }
            }
        }
    }

    return bestPos;
}

CaretGeometry TextHitTester::positionToGeometry(const Page& page, const DocumentPosition& pos) {
    CaretGeometry geom;
    if (!pos.isValid()) return geom;

    for (const auto& obj : page.objects()) {
        if (!obj || obj->type() != ObjectType::TextFrame || obj->id() != pos.frameId) continue;
        auto frame = std::static_pointer_cast<TextFrame>(obj);

        if (pos.paragraphIndex >= frame->paragraphs().size()) break;
        const auto& para = frame->paragraphs()[pos.paragraphIndex];

        if (pos.runIndex >= para.runs().size()) break;
        const auto& run = para.runs()[pos.runIndex];

        const Point base = getRunBaseline(run);
        const float charAdv = getCharAdvance(run);
        const float fontSize = run.style().fontSize > 0.0f ? run.style().fontSize : 12.0f;

        const float x = base.x + static_cast<float>(pos.charOffset) * charAdv;
        geom.height = fontSize;
        geom.position = Point{x, base.y};
        geom.top = Point{x, base.y - fontSize * 0.8f};
        geom.bottom = Point{x, base.y + fontSize * 0.2f};
        return geom;
    }

    return geom;
}

std::vector<Rect> TextHitTester::selectionToRectangles(const Page& page,
                                                      const DocumentPosition& start,
                                                      const DocumentPosition& end) {
    std::vector<Rect> rects;
    if (!start.isValid() || !end.isValid()) return rects;

    // Determine ordering
    DocumentPosition p0 = start;
    DocumentPosition p1 = end;
    bool swap = false;
    if (p0.paragraphIndex > p1.paragraphIndex) {
        swap = true;
    } else if (p0.paragraphIndex == p1.paragraphIndex) {
        if (p0.runIndex > p1.runIndex) {
            swap = true;
        } else if (p0.runIndex == p1.runIndex) {
            if (p0.charOffset > p1.charOffset) {
                swap = true;
            }
        }
    }
    if (swap) std::swap(p0, p1);

    for (const auto& obj : page.objects()) {
        if (!obj || obj->type() != ObjectType::TextFrame || obj->id() != p0.frameId) continue;
        auto frame = std::static_pointer_cast<TextFrame>(obj);

        for (std::size_t pi = p0.paragraphIndex; pi <= p1.paragraphIndex && pi < frame->paragraphs().size(); ++pi) {
            const auto& para = frame->paragraphs()[pi];
            const std::size_t startRun = (pi == p0.paragraphIndex) ? p0.runIndex : 0;
            const std::size_t endRun = (pi == p1.paragraphIndex) ? p1.runIndex : (para.runs().empty() ? 0 : para.runs().size() - 1);

            for (std::size_t ri = startRun; ri <= endRun && ri < para.runs().size(); ++ri) {
                const auto& run = para.runs()[ri];
                const Point base = getRunBaseline(run);
                const float charAdv = getCharAdvance(run);
                const float fontSize = run.style().fontSize > 0.0f ? run.style().fontSize : 12.0f;

                const std::size_t cStart = (pi == p0.paragraphIndex && ri == p0.runIndex) ? p0.charOffset : 0;
                const std::size_t cEnd = (pi == p1.paragraphIndex && ri == p1.runIndex) ? p1.charOffset : run.textUtf8().size();

                if (cStart >= cEnd) continue;

                const float x0 = base.x + static_cast<float>(cStart) * charAdv;
                const float x1 = base.x + static_cast<float>(cEnd) * charAdv;
                const float y0 = base.y - fontSize * 0.82f;
                const float y1 = base.y + fontSize * 0.22f;

                rects.push_back(Rect{
                    std::min(x0, x1),
                    std::min(y0, y1),
                    std::max(x0, x1),
                    std::max(y0, y1)
                });
            }
        }
    }

    return rects;
}

}  // namespace rpfg
