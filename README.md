# Rpfg

A PDF editing app built as a single native Windows executable: the UI is HTML/CSS/JS,
the engine is C++ (MuPDF) running on a worker thread pool, and the two are joined by a
WebView2 message bridge.

This milestone covers **open, render and scroll**, **selectable/copyable text**, and
**in-place text editing with instant save**. The concurrency foundation (worker pool,
display-list cache, priority scheduling) is what makes all of it responsive. Richer
editing — images, reflow, undo — is the next phase.

```
+---------------------------------------------------------------+
|  Rpfg.exe                                                     |
|                                                               |
|  UI thread                    render pool (N workers)         |
|  +---------------------+      +---------------------------+   |
|  | Win32 window        |      | worker 0 .. worker N-1    |   |
|  | WebView2 (Chromium) |      | own fz_context each       |   |
|  |   index.html + JS   |      | rasterise display lists   |   |
|  +----------+----------+      +------------+--------------+   |
|             |  JSON messages               |                  |
|  +----------v------------------------------v--------------+   |
|  | App: WebView2 host, JSON bridge, file dialogs          |   |
|  +-------------------------+------------------------------+   |
|                            |                                  |
|  +-------------------------v------------------------------+   |
|  | PdfEngine: document, display lists, caches, scheduling |   |
|  +-------------------------+------------------------------+   |
|                            |                                  |
|  +-------------------------v------------------------------+   |
|  | MuPDF (static libmupdf)                                |   |
|  +--------------------------------------------------------+   |
+---------------------------------------------------------------+
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the threading model, the locking
rules, the scheduling algorithm and the message protocol.

## Requirements

| Requirement | Notes |
| --- | --- |
| Windows 10 1809+ | WebView2 needs 10/11 |
| Visual Studio with **Desktop development with C++** | any toolset; the scripts retarget MuPDF automatically |
| CMake 3.20+ | on `PATH` |
| Ninja | on `PATH` |
| PowerShell 5.1+ | ships with Windows |
| Microsoft Edge WebView2 Runtime | preinstalled on Windows 10/11 |
| ~2 GB free disk | MuPDF source plus its build tree |

Everything else — MuPDF, the WebView2 SDK, `nlohmann/json` — is downloaded into
`third_party/` (git-ignored) on first build.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

That fetches the dependencies, builds `libmupdf` (several minutes the first time), then
compiles the app to `build\Rpfg.exe` with the `web` folder staged beside it.

```powershell
# build and launch
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Run

# clean rebuild of the application only
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean

# force a MuPDF rebuild (after editing third_party or switching toolchains)
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -RebuildMuPdf
```

Individual steps are available as `scripts\fetch_deps.ps1` and
`scripts\build_mupdf.ps1`.

## Trying it without a document

`docs\make_test_pdf.py` writes a small well-formed multi-page PDF (standard library
only) so you can exercise the viewer immediately:

```powershell
python docs\make_test_pdf.py "$env:TEMP\test.pdf"
.\build\Rpfg.exe "$env:TEMP\test.pdf"
```

The executable also writes `rpfg.log` beside itself, which logs the worker count, the
served web root, each document it opens, and a summary of the engine counters on exit.

## Usage

- **Open PDF** button, `Ctrl+O`, or drop a file onto the window
- `Rpfg.exe "C:\path\to\file.pdf"` opens a document directly
- Select text with the mouse and copy it with `Ctrl+C`; `Ctrl+A` selects the whole document
- **Edit text** — with the tool on, **a single click puts a caret in the text** and you type
  in place (no double-click, no floating box). Drag across a paragraph to edit it as a block
  that **reflows**: the words re-wrap to the paragraph's width keeping its left margin and
  leading. `Ctrl+Enter` or clicking away applies, `Escape` cancels. The editor sits on the
  page in the run's own font, size and colour, so it reads as the document
- **Line spacing** — the ▲/▼ buttons in the toolbar widen or tighten the space between
  lines (1.0×–3.0×); they drive the paragraph being edited live and set the default for the
  next one
- **Objects** — the page is parsed into real objects (lines, table rules, shapes,
  images, shades). Click one to select it and press `Delete` to remove it
- **Add text** — click on the page to place new text
- **Erase** — drag a box to delete everything it covers: shapes, lines, table rules,
  images and text. This is a real redaction (the content is removed from the file, not
  painted over)
- **Font / size / colour** apply to inserted and edited text. The font list is the PDF
  base-14 plus every TrueType/OpenType face found in a `fonts` folder next to `Rpfg.exe`
  and in the Windows font directory — so Times New Roman, Arial, Georgia, Calibri and
  friends all show up. Drop your own `.ttf`/`.otf` files into `fonts\` to add more
- **Save** (`Ctrl+S`) writes instantly with an incremental update — only the changed
  objects are appended, so it is fast even on large files. **Save As** (`Ctrl+Shift+S`)
  rewrites the file in full
- Zoom with the toolbar buttons, `Ctrl` + scroll, `Ctrl` + `+`/`-`, `Ctrl+0` to fit width
- The **Pages** rail lists every page and marks it once it has been rasterised
- The **stats** checkbox shows live engine counters (threads, jobs queued/submitted/
  coalesced/cancelled, cache hits and size) — the clearest way to see the concurrency
  doing its job while you scroll
- `F12` opens DevTools for the UI

## Layout

```
CMakeLists.txt            build definition for the app
scripts/
  config.ps1              shared paths, pinned versions, VS/MSBuild discovery
  fetch_deps.ps1          MuPDF source, WebView2 SDK, nlohmann/json
  build_mupdf.ps1         builds libmupdf via the shipped VS solution
  build.ps1               one-shot: deps -> MuPDF -> app
src/
  main.cpp                wWinMain, COM/DPI setup, exception boundary
  app/                    Win32 window, WebView2 host, JSON bridge
  pdf/                    PdfEngine: MuPDF, thread pool, caches, scheduling
  util/                   thread pool, logging, UTF-8/16, base64
web/
  index.html              UI shell
  css/app.css             dark theme
  js/bridge.js            WebView2 message channel
  js/viewer.js            page layout, viewport reporting, canvas lifecycle
  js/main.js              toolbar wiring, keyboard shortcuts, stats
docs/ARCHITECTURE.md      design notes
docs/make_test_pdf.py     generates a throwaway multi-page PDF for testing
tests/text_geometry_test  asserts the extracted-text coordinate convention
tests/edit_save_test      edits, saves (full + incremental) and re-reads the text
third_party/              fetched dependencies (git-ignored)
```

## Licensing

**MuPDF is AGPL-3.0** (or a paid commercial licence from Artifex). Because Rpfg links
MuPDF statically, distributing this application obliges you to release its source under
the AGPL as well. That is fine for local and personal use; if you intend to ship it
closed-source, either buy a commercial MuPDF licence or swap the engine for PDFium
(BSD-3), which is a contained change — `PdfEngine` is the only file that touches MuPDF.

The WebView2 SDK and `nlohmann/json` (MIT) are both permissively licensed.
