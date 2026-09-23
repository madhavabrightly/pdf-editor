# Rpfg architecture

Two runtimes with very different natures share one process: a Chromium (WebView2) UI
that must stay responsive, and a C rendering engine that is fast but single-threaded per
context. Nearly every design decision here follows from keeping the first one free while
saturating the second.

## 1. Threads

| Thread | Owns | Never does |
| --- | --- | --- |
| UI thread | Win32 message loop, WebView2, `App`, the JSON bridge, `PdfEngine::openDocument` / `requestViewport` | rasterise, touch MuPDF documents |
| `rpfg-render-0..N-1` | one `fz_context` each, rasterisation | anything COM/WebView2, anything UI |

Pool size is `clamp(hardware_concurrency, 2, 12)`, minus one when it exceeds 4 so the UI
thread and the compositor keep a core. Rasterising a page is CPU-bound and parallel, so
the pool is worth saturating; three quarters of a core goes to the UI.

### One MuPDF context per thread

`fz_context` is not thread-safe. The engine creates a parent context with a shared
`fz_locks_context`, then each worker clones it with `fz_clone_context`. Clones share the
resource store and the locks, so objects (documents, display lists, fonts, pixmaps) can
be handed between contexts safely — but each thread needs its own context for its own
error stack and glyph cache.

`ThreadPool` hands each worker its index on start, so the engine keeps a
`std::vector<fz_context*>` indexed by worker rather than resorting to `thread_local`.

## 2. Display lists are the pivot

The single most important decision: **a page is converted to a display list once, and
that display list is rasterised many times.**

| | cost | depends on zoom? | thread-safe to read? |
| --- | --- | --- | --- |
| page → display list | high — parses content, resolves fonts/images | no | n/a (build once) |
| display list → pixmap | moderate | yes | yes, from many threads at once |

Consequences:

- Zooming re-runs only the raster step. Dragging the zoom slider reuses the expensive
  work, which is why zoom feels cheap after the first render.
- `documentMutex` is held only while building a display list. Rasterisation never touches
  the document, so it parallelises perfectly.
- A display list holds its own references to fonts, images and colours, so it stays valid
  even if the document is closed underneath it.

## 3. Locks

| Lock | Guards | Held while |
| --- | --- | --- |
| `documentMutex` | `fz_document`, page loading, display-list construction, editing and saving | short, never during rasterisation |
| `displayListMutex` | display-list cache | map operations only |
| `bitmapMutex` | base64 PNG cache | map operations only |
| `textMutex` | extracted text-layer cache (page-keyed) | map operations only |
| `wantedMutex` | the set of keys the UI currently wants, and which pages' text it already holds (`textDelivered`) | set replacement |
| `textInflightMutex` | pages whose text is being extracted | set operations |
| `inflightMutex` | keys with a queued/running job | set operations |

Rules:

- Never rasterise while holding `documentMutex` — that would serialise the pool.
- `wantedMutex` is the outermost of the text locks: code may take `wantedMutex` then
  `textMutex` (see `requestViewport` resending cached text), never the reverse.
- Never hold two of the above if a MuPDF lock is also involved, in a different order on
  different threads. MuPDF requires its own `FZ_LOCK_ALLOC` → `FZ_LOCK_FREETYPE` →
  `FZ_LOCK_GLYPHCACHE` ordering; the app's mutexes are only ever taken in the order
  listed above, and released in reverse, so no cycle exists.
- Workers never touch `info` (page count/sizes); page geometry is captured by value when
  a job is queued.

## 4. Scheduling

The UI does not request individual pages. On every scroll or zoom change it reports the
*set* of pages that are within 1.5 viewports of the screen, plus the pixel scale. The
engine does the rest:

1. Quantise the scale to 0.25 steps and key every unit of work on `(page, scaleSteps)`.
2. Serve any key already in the bitmap cache immediately — no queueing at all.
3. Skip keys that already have a job in flight (`inflight`), counted as *coalesced*.
4. Submit the rest to the priority queue, ordered by distance from the top of the viewport.
5. Replace `wanted` with the new set.

`wanted` is what makes fast scrolling cheap. A job that is already rasterising a page that
is *still* on screen is never wasted, because its key remains in `wanted`. A job for a page
that scrolled away finds its key gone from `wanted` and abandons itself — checked both
before touching the document and again before delivery. There is no generation counter to
get out of step and no queue to flush: the desired state is simply *replaced*, and stale
work discovers it is stale on its own.

This is why the UI can emit a viewport message on every animation frame for free; identical
consecutive sets are filtered in JS, and non-identical ones are idempotent in the engine.

## 5. Memory

Two independent budgets, on opposite sides of the bridge:

- **Engine**: rendered PNGs are cached base64-encoded (the exact form the bridge needs, so
  each raster is encoded once) in an LRU cache capped at 192 MB, evicted by least-recent
  use. Base64 costs 33% over raw PNG and buys zero re-encoding.
- **UI**: canvases more than 3 viewports away are resized to 0×0, which drops the decoded
  bitmap. Scrolling back within the engine's cache window therefore re-delivers a cached
  raster, and the bridge payload never needs to be recreated.

Raster dimensions are clamped to 8192 px on the long edge, so a stray zoom level on an A0
drawing cannot ask MuPDF for a multi-gigabyte pixmap.

## 6. C++ and `fz_try`

MuPDF signals errors with `fz_throw`, implemented as `longjmp`. That has one hard
consequence for C++ callers:

- **No object with a destructor may be constructed inside `fz_try`.** `longjmp` does not
  unwind, so destructors (and the associated mutex unlocks) would be skipped.
- Locks are therefore acquired *outside* `fz_try`, in a scope that encloses both `fz_try`
  and `fz_catch`. Normal control flow reaches `fz_catch`, so the lock is always released.
- Anything that allocates C++ heap memory — `std::vector::push_back`, `std::string`
  assignment — happens outside `fz_try`, so `std::bad_alloc` cannot unwind through MuPDF's
  try stack and leave it imbalanced.
- `fz_var` marks every local that is written inside `fz_try` and read in `fz_always` /
  `fz_catch`, so the compiler cannot keep it in a register across `setjmp`.

`openDocument` shows the pattern: open and count inside one `fz_try`, size the geometry
vector outside it, then fill it in a second `fz_try`.

## 7. Message protocol

Lives in `src/app/App.cpp` (`onWebMessage`, `publish*`). JSON both ways; the host parses
with `nlohmann/json` and hand-writes the page payloads, which are multi-megabyte base64
strings that a generic serializer would copy several times.

### UI → host

| Message | Effect |
| --- | --- |
| `{"cmd":"ready"}` | UI is listening; host replies `hostReady` and flushes any file it was launched with |
| `{"cmd":"openDialog"}` | native open dialog, then load |
| `{"cmd":"openPath","path":"…"}` | load a path directly |
| `{"cmd":"viewport","pages":[0,1,2],"scale":1.5}` | the pages to render and the current pixel scale |
| `{"cmd":"editText",…}` | `page`, `x`, `y`, `s`, `a`, `font`, `r`/`g`/`b`, `replace`, `text` (and `w`/`asc` when replacing) — edit or insert text |
| `{"cmd":"editBlock",…}` | `page`, `x0`/`y0`/`x1`/`y1`, `font`, colour, `text`, `lines:[{x,y,s}]` — edit a multi-line selection |
| `{"cmd":"eraseRegion","page":0,"x0":…,"y0":…,"x1":…,"y1":…}` | delete everything drawn inside a page-space rectangle |
| `{"cmd":"save"}` | incremental save to the current path |
| `{"cmd":"saveAs"}` | rewrite the file in full (host prompts for a path) |
| `{"cmd":"saveCopy"}` | rewrite a copy, leaving the document's own path unchanged |
| `{"cmd":"stats"}` | request engine counters |

`scale` is **device pixels per PDF point** (`zoom × devicePixelRatio`), and the host
quantises it to 0.25 steps.

### Host → UI

| Message | Payload |
| --- | --- |
| `{"type":"hostReady"}` | handshake complete |
| `{"type":"document",…}` | `path`, `title`, `pageCount`, `threads`, `pages:[{width,height}]` in points, `fonts:[{id,label,embedded}]` |
| `{"type":"page",…}` | `page`, `scale`, `width`, `height`, `image:"data:image/png;base64,…"` |
| `{"type":"text",…}` | `page`, `data:{"runs":[{"x","y","s","w","a","asc","c","f","fid","t"}]}` in PDF points |
| `{"type":"edited","page":…}` | an edit was applied; re-request that page |
| `{"type":"saved","path":…,"incremental":…}` | the document was written |
| `{"type":"stats",…}` | engine counters |
| `{"type":"error","message":…}` | surfaced as a toast |

### The text layer

Selection and copy are done by the browser, not by the app. For every visible page the
engine turns its display list into an `fz_stext_page`, walks the lines, and emits one run
per line: baseline origin `(x, y)`, font size `s`, baseline advance `w`, baseline angle `a`,
font ascender `asc` (fraction of `s`) and the UTF-8 text `t`. Coordinates are PDF points with
a **top-left origin, y growing downward** — the same convention `fz_stext` uses, and the one
`tests/text_geometry_test.cpp` pins down against a known-geometry PDF.

The UI builds an invisible `.text-layer` per page: absolutely-positioned spans holding real
DOM text, laid out in PDF points and scaled as a whole with `transform: scale(zoom)`, so
zooming never repositions a run. Each run is shifted from the baseline to the top of its box
with `asc`, rotated by `a`, and stretched with `scaleX(w / measuredWidth)` so the browser's
font occupies the same width the PDF font did. Because the glyphs are real text, native
selection, `Ctrl+A` and `Ctrl+C` all work with no clipboard code.

Unlike rasters, text is keyed by page alone: extraction is zoom-independent, so it is cached
once per page and never invalidated by zoom. The UI drops layers for pages more than three
viewports away (same budget as canvases); `requestViewport` notices via `textDelivered` and
re-sends a cached layer when the page scrolls back.

### Editing and saving

Editing reuses the text layer's geometry in reverse. The UI hands back the run it edited
(origin, size, advance, angle, ascender, new text); the engine loads the page and:

1. **Erases the old glyphs** with a single-use Redact annotation whose rect is the run's
   box, applied with `PDF_REDACT_IMAGE_NONE` and `PDF_REDACT_LINE_ART_NONE`. The redaction
   pass rewrites the content stream, physically removing exactly the characters whose
   quads intersect the box — it is not a cover-up, so a later search or copy cannot find
   the old text — while leaving images and vector art untouched.
2. **Draws the replacement** by appending a new content stream that selects a standard
   Helvetica font resource on the page and emits one `Tj`. PDF user space is bottom-left
   origin, so the run's top-down `y` becomes `pageHeight - y`; flipping the axis conjugates
   the run angle, which is why the text matrix's linear part is `[c -s s c]` and not the
   usual `[c s -s c]`.

**Matching the document.** A run's geometry comes from the structured-text pass, but that
pass carries no colour and no font — so each display list is *also* replayed through a tiny
`fill_text` probe device (`pdf_probe.c`-style: a `fz_device` whose only non-stub callback is
`fill_text`). Each span it sees is recorded with its origin, fill colour (converted to RGB
with `fz_convert_color`) and `fz_font_name`. Because a span's origin is
`transform(item[0].x/y, ctm)` — exactly the point the stext run reports — the two lists line
up by position and the colour/font is attached to each run. The PDF font name is matched
against the available faces (subset prefix and punctuation stripped) so an edit reuses the
document's own face; the file is then served to the UI over a `fonts.local` /
`winfonts.local` virtual host and loaded with `@font-face`. The editing overlay paints the
run in its own colour and face over an opaque box, so while you type it looks like the page
itself.

**Objects.** The same probe captures geometry too: `fill_path` / `stroke_path` (bounded with
`fz_bound_path`, stroke state included), images and image masks (the unit square transformed
by the ctm) and shades (`fz_bound_shade`). Each becomes a bounding box tagged with a kind,
filtered to drop specks and absurd sizes and capped at 4000 per page. They travel with the
text message as `objects` and are drawn as a transparent overlay in PDF points, so the
Objects tool can turn a click into a selection and `Delete` into a redaction over exactly
that box — a table rule or a shape is selected as itself, not guessed from a freehand drag.

**Fonts.** Standard faces (the PDF base-14: Helvetica, Times, Courier, Symbol,
ZapfDingbats, with their bold/italic variants) are referenced by name and need nothing
embedded. Anything else — Times New Roman, Arial, Georgia, Calibri, plus whatever the user
drops into a `fonts` folder or has installed in Windows — is loaded with
`fz_new_font_from_file` and embedded through `pdf_add_cid_font`, which builds an
Identity-H font with a `/W` widths array and a ToUnicode map derived from the font's own
cmap. Text drawn with it is encoded as two-byte glyph ids from `fz_encode_character`, so it
still searches and copies correctly. Each embedded face is created once per document and
cached.

**Erase.** The erase tool drags a rectangle and applies a redaction over it with images
(`PDF_REDACT_IMAGE_REMOVE`) and line art (`PDF_REDACT_LINE_ART_REMOVE_IF_COVERED`) switched
on as well as text — so shapes, table rules, images and glyphs whose geometry the box covers
are deleted from the content stream. `black_boxes` is off, so nothing is painted in their
place. This is deletion, not a vector editor: it cannot reshape a path, only remove it.

**Paragraph editing.** Editing more than one run works the same way, once: the UI drags a
box, collects every run it intersects, groups them into lines by baseline, and hands the
engine the union box plus each line's left edge, baseline and size. The engine erases the
union with one text-only redaction and redraws the text line by line on those original
baselines, so a paragraph keeps its left margin and leading; lines added beyond the original
count continue below at the last line's 1.2× leading. A `spacing` multiplier scales each
line's offset from the first baseline, so leading can be widened or tightened without moving
the top of the block (`Alt`+`↑`/`↓` in the editor). Spaces and blank lines the user types
are preserved (the editor is a `white-space: pre-wrap` box), which is what makes menu lists
and columns survive an edit.

**Reflow.** When the selection is a run of same-size lines — a real paragraph — the edit
*reflows* like a word processor. The wrapping is done in the UI, not the engine, on purpose:
the browser has the document's actual face loaded (via `@font-face`, see below), so
`measureTextWidth` gives true advance widths. The edited text is word-wrapped to the
paragraph's width, each resulting line is placed on a baseline one leading below the last,
and the engine is simply handed the wrapped lines — the same explicit-line path as above.
Mixed-size selections (a heading over body text) are not forced together; they keep their own
lines. This is deliberately *not* full document retypesetting: only the selected paragraph
re-lays-out, and the document's own operators are replaced by the redraw rather than
rewritten in place.

**Editing in place.** The editor is not a box. With the Edit tool, a single click drops a
caret into the run (no double-click, no chrome); a drag opens a borderless overlay sized to
the paragraph, filled with the page's own background colour (sampled from the raster), set in
the run's own font, size and colour, with `line-height` equal to the paragraph's leading.
Typing looks like editing the page. The toolbar ▲/▼ spacing buttons drive the open editor's
`line-height` live and set the default for the next one.

Because the page's content stream changed, every derived object is dropped
(`invalidatePage`): the display list, all rasters keyed on `(page, scale)`, the text layer,
and the `textDelivered` record so the next viewport report re-sends the newly extracted
text. The UI then re-renders just that page.

Saving is `pdf_save_document`:

- **Incremental** (`do_incremental`) appends only the changed objects to the existing file
  — the reason "save on every edit" is affordable — and is used by **Save**. It requires
  the file to have been opened without repair; if the append fails the engine falls back to
  a full rewrite automatically.
- **Full** (`do_garbage`, `do_compress`) rewrites and compacts the whole file, used by
  **Save As** / **Save a copy**. Writing in full *to the file currently open* goes via a
  temp file and an atomic `MoveFileEx` replace.

Once a full save has happened the incremental path is disabled (`fullSaveDone`): the
in-memory cross-reference no longer matches the file it was opened from, so an append would
be wrong. The engine tracks a dirty flag; the host shows it as an asterisk in the title and
prompts on close.

### Why a virtual host, not `file://`

`SetVirtualHostNameToFolderMapping` maps the `web` folder to `https://app.local/`. The UI
is then a real secure origin, so ES modules, `fetch`, service workers and DevTools all
behave normally — none of which work reliably from `file://`.

## 8. Deliberate deferrals

Known limits of this milestone, and what fixing them involves:

- **PNG + base64 across the bridge.** Roughly 33% transport overhead and a PNG encode per
  raster. `PostSharedBufferToScript` would move raw pixels with zero copies; it needs the
  page to opt into shared buffers. Worth doing once tiles land.
- **One pixmap per page, no tiling.** Fine at 100–200%, wasteful at 800% on a large page
  where only a corner is visible. Tiling means keying the cache on `(page, scale, tile)`.
- **No text search.** The extracted text layer (section 7) already holds every run's text
  and position, so search is a matter of matching runs and scrolling to them; there is just
  no search UI yet.
- **Editing is not a layout engine.** Text edits replace a run's glyphs or insert new text;
  they do not reflow a paragraph. Graphics can be erased (see above) but not reshaped,
  moved as objects, or grouped — PDF has no shape/table objects, only path operators, so
  there is nothing to select as a "table". Standard fonts are WinAnsi-encoded (Latin);
  embedded faces go through the font's own cmap and so carry whatever the face supports.
  There is still no undo — an edit is applied immediately and permanently.
- **No thumbnails in the rail.** The infrastructure is already there — queue low-priority
  jobs at a small scale for every page; the priority queue keeps them behind visible
  pages, and `wanted` would need a second, lower-priority band.
