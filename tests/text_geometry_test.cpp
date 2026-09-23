// Exercises the engine's text extraction against a PDF whose geometry is known
// exactly (produced by docs/make_test_pdf.py) and prints the runs it finds.
//
// This exists because the coordinate convention of extracted text is the one
// thing about the text layer that cannot be checked by reading code: MuPDF
// reports positions in page space, and the UI has to agree with it exactly or
// every selection rectangle lands in the wrong place.

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pdf/PdfEngine.h"

namespace {

std::mutex g_mutex;
std::condition_variable g_ready;
std::vector<nlohmann::json> g_pages;

// The generator draws this with a 36pt font at PDF user space (92, 700) on a
// 612x792 page, so 92 points from the left and 92 points down from the top.
constexpr double kHeadingX = 92.0;
constexpr double kHeadingTopY = 792.0 - 700.0;
constexpr double kHeadingBottomY = 700.0;
constexpr double kHeadingSize = 36.0;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rpfg_text_test <file.pdf>\n");
        return 2;
    }

    rpfg::PdfEngine engine;
    engine.setPageTextCallback([](std::shared_ptr<const rpfg::PageText> text) {
        std::lock_guard<std::mutex> guard(g_mutex);
        g_pages.push_back(nlohmann::json::parse(*text->runs));
        g_ready.notify_all();
    });

    std::string error;
    const std::optional<rpfg::DocumentInfo> info = engine.openDocument(argv[1], error);
    if (!info.has_value()) {
        std::fprintf(stderr, "FAIL: could not open %s: %s\n", argv[1], error.c_str());
        return 1;
    }
    std::printf("opened %d-page document\n", info->pageCount);

    engine.requestViewport({0}, 1.0f);

    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_ready.wait_for(lock, std::chrono::seconds(60), [] { return !g_pages.empty(); })) {
            std::fprintf(stderr, "FAIL: no text layer arrived within 60s\n");
            return 1;
        }
    }

    const nlohmann::json& page = g_pages.front();
    if (!page.contains("runs") || !page["runs"].is_array() || page["runs"].empty()) {
        std::fprintf(stderr, "FAIL: text layer contained no runs\n");
        return 1;
    }

    std::printf("page 0: %zu run(s)\n", page["runs"].size());
    int printed = 0;
    for (const auto& run : page["runs"]) {
        if (printed++ >= 6) {
            break;
        }
        std::printf("  x=%8.3f  y=%8.3f  size=%7.3f  advance=%8.3f  angle=%8.5f  asc=%6.3f  "
                    "rgb=(%.2f,%.2f,%.2f)  font=\"%s\"  fid=\"%s\"  \"%s\"\n",
                    run["x"].get<double>(), run["y"].get<double>(), run["s"].get<double>(),
                    run["w"].get<double>(), run["a"].get<double>(), run["asc"].get<double>(),
                    run["c"][0].get<double>(), run["c"][1].get<double>(),
                    run["c"][2].get<double>(), run["f"].get<std::string>().c_str(),
                    run["fid"].get<std::string>().c_str(),
                    run["t"].get<std::string>().c_str());
    }

    // Every run must carry a fill colour and a (possibly empty) matched font id,
    // which is what the editor uses to edit in the document's own look.
    for (const auto& run : page["runs"]) {
        if (!run.contains("c") || !run["c"].is_array() || run["c"].size() != 3 ||
            !run.contains("fid")) {
            std::fprintf(stderr, "FAIL: run is missing colour or font id\n");
            return 1;
        }
    }
    std::printf("colour/font metadata: present\n");

    // The page must also be parsed into geometry objects (here: the stroked frame
    // and the filled bar the generator draws).
    if (!page.contains("objects") || !page["objects"].is_array() || page["objects"].empty()) {
        std::fprintf(stderr, "FAIL: no geometry objects were parsed\n");
        return 1;
    }
    std::printf("geometry objects: %zu\n", page["objects"].size());

    bool foundHeading = false;
    for (const auto& run : page["runs"]) {
        const std::string text = run["t"].get<std::string>();
        if (text.find("Rpfg test") == std::string::npos) {
            continue;
        }
        foundHeading = true;

        const double x = run["x"].get<double>();
        const double y = run["y"].get<double>();
        const double size = run["s"].get<double>();

        if (std::fabs(x - kHeadingX) > 2.0) {
            std::fprintf(stderr, "FAIL: heading x is %.3f, expected ~%.1f\n", x, kHeadingX);
            return 1;
        }
        if (std::fabs(size - kHeadingSize) > 1.0) {
            std::fprintf(stderr, "FAIL: heading size is %.3f, expected ~%.1f\n", size, kHeadingSize);
            return 1;
        }

        const bool topLeft = std::fabs(y - kHeadingTopY) <= 2.0;
        const bool bottomLeft = std::fabs(y - kHeadingBottomY) <= 2.0;
        if (!topLeft && !bottomLeft) {
            std::fprintf(stderr,
                         "FAIL: heading y is %.3f, which is neither the top-left (%.1f) nor the "
                         "bottom-left (%.1f) convention\n",
                         y, kHeadingTopY, kHeadingBottomY);
            return 1;
        }
        std::printf("heading: x=%.1f  y=%.1f  size=%.1f\n", x, y, size);
        std::printf("coordinate convention: %s\n",
                    topLeft ? "top-left origin, y grows downward"
                            : "bottom-left origin, y grows upward");
    }

    if (!foundHeading) {
        std::fprintf(stderr, "FAIL: could not find the heading text in the extracted runs\n");
        return 1;
    }

    std::printf("PASS\n");
    return 0;
}
