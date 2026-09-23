import * as bridge from "./bridge.js";

// The UI never rasterises anything itself. It lays pages out at the requested
// zoom, works out which pages are close enough to be worth pixels, and tells
// the C++ engine. Everything about *how* those pixels are produced - thread
// pool, display lists, caching, cancellation - lives on the other side.

const ZOOM_MIN = 0.25;
const ZOOM_MAX = 4;
const ZOOM_STEP = 0.25;

// How far beyond the viewport we ask for rasters, and how far beyond that we
// keep already-decoded canvases alive. Canvases are cleared outside the second
// window so a long document does not hold every page bitmap in memory.
const RENDER_AHEAD_SCREENS = 1.5;
const KEEP_PIXELS_SCREENS = 3;

// Must match .text-layer's font-family: the text is expected to be laid out in a
// different face than the PDF used, so runs are stretched to the PDF's width.
const TEXT_LAYER_FONT = "sans-serif";

let measureContext = null;

function parseColor(value) {
  try {
    const parsed = JSON.parse(value || "[0,0,0]");
    if (Array.isArray(parsed) && parsed.length === 3) {
      return parsed.map((channel) => Math.min(1, Math.max(0, Number(channel) || 0)));
    }
  } catch (error) {
    /* fall through to black */
  }
  return [0, 0, 0];
}

function measureTextWidth(text, fontSize, family = TEXT_LAYER_FONT) {
  if (measureContext === null) {
    measureContext = document.createElement("canvas").getContext("2d");
  }
  measureContext.font = `${fontSize}px ${family}`;
  return measureContext.measureText(text).width;
}

// Word-wraps `text` to `maxWidth`, measuring with the real font, so an edited
// paragraph reflows the way a word processor would. Explicit newlines force a
// break. Returns an array of lines.
function wrapText(text, fontSize, family, maxWidth) {
  const result = [];
  for (const paragraph of text.split("\n")) {
    const words = paragraph.split(/(\s+)/).filter((token) => token !== "");
    let line = "";
    let lineWidth = 0;
    for (const word of words) {
      const width = measureTextWidth(word, fontSize, family);
      const isSpace = /^\s+$/.test(word);
      if (line !== "" && !isSpace && lineWidth + width > maxWidth) {
        result.push(line.replace(/\s+$/, ""));
        line = word;
        lineWidth = width;
      } else {
        line += word;
        lineWidth += width;
      }
    }
    result.push(line.replace(/\s+$/, ""));
  }
  return result;
}

export class Viewer {
  constructor({ viewport, pages, rail, onZoomChange, onPageChange, onEditRun, onEditBlock, onInsertText, onErase, onResolveFont }) {
    this.viewport = viewport;
    this.pagesEl = pages;
    this.railEl = rail;
    this.onZoomChange = onZoomChange;
    this.onPageChange = onPageChange;
    this.onEditRun = onEditRun;
    this.onEditBlock = onEditBlock;
    this.onInsertText = onInsertText;
    this.onErase = onErase;
    this.onResolveFont = onResolveFont;

    // Active tool: "select" | "edit" | "addtext" | "erase". Double-click editing
    // is always available; the tool decides what a single click / drag does.
    this.tool = "select";
    this.eraseStart = null;
    this.selectStart = null;
    this.marquee = null;
    this.selectedObject = null;
    this.initialSpacing = 1;
    this.adjustSpacing = null;
    this.setSpacing = null;

    this.hasDocument = false;
    this.pageCount = 0;
    this.pageSizes = [];
    this.elements = [];
    this.railItems = [];
    this.offsets = [];
    this.heights = [];

    this.zoom = 1;
    this.dpr = window.devicePixelRatio || 1;
    this.currentPage = -1;
    this.lastRequest = null;
    this.frameHandle = 0;

    this.viewport.addEventListener("scroll", () => this.scheduleUpdate(), { passive: true });
    window.addEventListener("resize", () => this.handleResize());
    this.watchDevicePixelRatio();
    this.installToolHandlers();
  }

  // ------------------------------------------------------------------ document

  loadDocument(message) {
    this.hasDocument = true;
    this.pageCount = message.pageCount;
    this.pageSizes = (message.pages || []).map((page) => ({
      width: Number(page.width) || 612,
      height: Number(page.height) || 792,
    }));

    this.buildDom();
    this.zoom = 1;
    this.applyLayout();
    this.measure();
    this.fitWidth();
    this.viewport.scrollTop = 0;
    this.currentPage = -1;
    this.lastRequest = null;
    this.measure();
    this.update();
  }

  clear() {
    this.hasDocument = false;
    this.pageCount = 0;
    this.pageSizes = [];
    this.elements = [];
    this.railItems = [];
    this.offsets = [];
    this.heights = [];
    this.currentPage = -1;
    this.lastRequest = null;
    this.pagesEl.replaceChildren();
    this.railEl.replaceChildren();
  }

  buildDom() {
    this.pagesEl.replaceChildren();
    this.railEl.replaceChildren();
    this.elements = [];
    this.railItems = [];

    for (let index = 0; index < this.pageCount; index += 1) {
      const wrapper = document.createElement("div");
      wrapper.className = "page";
      wrapper.dataset.state = "loading";

      const canvas = document.createElement("canvas");
      const textLayer = document.createElement("div");
      textLayer.className = "text-layer";
      const objectLayer = document.createElement("div");
      objectLayer.className = "object-layer";
      wrapper.append(canvas, textLayer, objectLayer);
      this.pagesEl.append(wrapper);
      this.elements.push({
        wrapper,
        canvas,
        textLayer,
        objectLayer,
        context: canvas.getContext("2d"),
        renderedScale: 0,
      });

      const item = document.createElement("button");
      item.type = "button";
      item.className = "page-rail__item";

      const number = document.createElement("span");
      number.className = "page-rail__number";
      number.textContent = String(index + 1);

      const dot = document.createElement("span");
      dot.className = "page-rail__dot";

      const size = document.createElement("span");
      size.className = "page-rail__size";
      const pageSize = this.pageSizes[index];
      size.textContent = `${Math.round(pageSize.width)}×${Math.round(pageSize.height)}`;

      item.append(number, dot, size);
      item.addEventListener("click", () => this.scrollToPage(index));
      this.railEl.append(item);
      this.railItems.push({ item, dot });
    }
  }

  // ------------------------------------------------------------------- layout

  applyLayout() {
    for (let index = 0; index < this.elements.length; index += 1) {
      const size = this.pageSizes[index];
      const { wrapper, textLayer, objectLayer } = this.elements[index];
      wrapper.style.width = `${Math.max(1, Math.round(size.width * this.zoom))}px`;
      wrapper.style.height = `${Math.max(1, Math.round(size.height * this.zoom))}px`;
      // Runs and objects are positioned in PDF points; scaling the whole layer
      // keeps them correct at every zoom level without touching a single entry.
      for (const layer of [textLayer, objectLayer]) {
        layer.style.width = `${size.width}px`;
        layer.style.height = `${size.height}px`;
        layer.style.transform = `scale(${this.zoom})`;
      }
    }
  }

  measure() {
    this.offsets = [];
    this.heights = [];
    for (const element of this.elements) {
      this.offsets.push(element.wrapper.offsetTop);
      this.heights.push(element.wrapper.offsetHeight);
    }
  }

  effectiveScale() {
    return Math.min(Math.max(this.zoom * this.dpr, 0.05), 8);
  }

  handleResize() {
    if (!this.hasDocument) {
      return;
    }
    this.measure();
    this.scheduleUpdate();
  }

  watchDevicePixelRatio() {
    // Dragging the window to a display with different scaling changes
    // devicePixelRatio; the rasters have to be produced again for the new
    // pixel density to stay crisp.
    const media = window.matchMedia(`(resolution: ${this.dpr}dppx)`);
    const listener = () => {
      this.dpr = window.devicePixelRatio || 1;
      this.lastRequest = null;
      this.handleResize();
      this.watchDevicePixelRatio();
    };
    media.addEventListener("change", listener, { once: true });
  }

  // ---------------------------------------------------------------- viewport

  scheduleUpdate() {
    if (this.frameHandle) {
      return;
    }
    this.frameHandle = requestAnimationFrame(() => {
      this.frameHandle = 0;
      this.update();
    });
  }

  update() {
    if (!this.hasDocument || this.elements.length === 0) {
      return;
    }

    const scrollTop = this.viewport.scrollTop;
    const height = this.viewport.clientHeight;
    const scale = this.effectiveScale();

    const ahead = height * RENDER_AHEAD_SCREENS;
    const keep = height * KEEP_PIXELS_SCREENS;
    const wanted = [];

    for (let index = 0; index < this.offsets.length; index += 1) {
      const top = this.offsets[index];
      const bottom = top + this.heights[index];

      if (bottom >= scrollTop - ahead && top <= scrollTop + height + ahead) {
        wanted.push(index);
      }
      if (bottom < scrollTop - keep || top > scrollTop + height + keep) {
        this.releasePixels(index);
      }
    }

    // Only talk to the engine when the request actually changes; scrolling
    // otherwise fires dozens of identical messages a second.
    const signature = `${scale.toFixed(4)}|${wanted.join(",")}`;
    if (signature !== this.lastRequest) {
      this.lastRequest = signature;
      bridge.send({ cmd: "viewport", pages: wanted, scale });
    }

    this.updateCurrentPage(scrollTop, height);
  }

  releasePixels(index) {
    const element = this.elements[index];
    if (!element) {
      return;
    }
    if (element.canvas.width !== 0) {
      element.canvas.width = 0;
      element.canvas.height = 0;
      element.renderedScale = 0;
      element.wrapper.dataset.state = "loading";
      this.updateRailState(index);
    }
    // Text layers are dropped too, to bound the DOM on long documents. The
    // engine's page-keyed cache re-sends it if the page scrolls back.
    if (element.textLayer.childElementCount > 0) {
      element.textLayer.replaceChildren();
    }
    if (element.objectLayer.childElementCount > 0) {
      element.objectLayer.replaceChildren();
    }
  }

  updateCurrentPage(scrollTop, height) {
    const centre = scrollTop + height / 2;
    let current = 0;
    for (let index = 0; index < this.offsets.length; index += 1) {
      if (this.offsets[index] <= centre) {
        current = index;
      } else {
        break;
      }
    }

    if (current === this.currentPage) {
      return;
    }
    this.currentPage = current;

    for (let index = 0; index < this.railItems.length; index += 1) {
      this.railItems[index].item.setAttribute("aria-current", index === current ? "true" : "false");
    }
    this.onPageChange?.(current, this.pageCount);
  }

  updateRailState(index) {
    const rail = this.railItems[index];
    if (!rail) {
      return;
    }
    const ready = this.elements[index]?.renderedScale > 0;
    rail.dot.classList.toggle("page-rail__dot--ready", ready);
    rail.dot.classList.toggle("page-rail__dot--pending", !ready);
  }

  // ------------------------------------------------------------------ results

  handleRenderedPage(message) {
    const index = message.page;
    const element = this.elements[index];
    if (!element) {
      return;
    }

    const image = new Image();
    image.decoding = "async";
    image.onload = () => {
      element.canvas.width = image.naturalWidth;
      element.canvas.height = image.naturalHeight;
      element.context.drawImage(image, 0, 0);
      element.renderedScale = message.scale;
      element.wrapper.dataset.state = "ready";
      this.updateRailState(index);
    };
    image.onerror = () => {
      element.wrapper.dataset.state = "failed";
      console.error(`[viewer] could not decode page ${index + 1}`);
    };
    image.src = message.image;
  }

  // Builds the selectable layer for a page. Runs arrive in PDF points with the
  // origin on the text baseline; the characters themselves are real DOM text, so
  // selection and copying are handled natively by the browser.
  handlePageText(message) {
    const index = message.page;
    const layer = this.elements[index]?.textLayer;
    if (!layer || !message.data || !Array.isArray(message.data.runs)) {
      return;
    }

    const fragment = document.createDocumentFragment();
    for (const run of message.data.runs) {
      const text = run.t;
      if (!text) {
        continue;
      }
      const size = run.s > 0 ? run.s : 1;

      const span = document.createElement("span");
      span.className = "text-run";
      span.textContent = text;
      span.style.fontSize = `${size}px`;
      span.style.left = `${run.x}px`;
      // Shift from the baseline to the top of the box, using the PDF font's own
      // ascender.
      span.style.top = `${run.y - run.asc * size}px`;

      // Everything needed to hand this run straight back to the engine when the
      // user edits it, so the edit needs no second lookup.
      span.dataset.page = String(index);
      span.dataset.x = String(run.x);
      span.dataset.y = String(run.y);
      span.dataset.w = String(run.w);
      span.dataset.s = String(run.s);
      span.dataset.a = String(run.a);
      span.dataset.asc = String(run.asc);
      span.dataset.c = JSON.stringify(Array.isArray(run.c) ? run.c : [0, 0, 0]);
      span.dataset.fid = run.fid || "";
      span.dataset.original = text;
      span.addEventListener("dblclick", (event) => {
        event.stopPropagation();
        this.beginEdit(span);
      });
      span.addEventListener("click", (event) => {
        if (this.tool !== "edit") {
          return;
        }
        event.stopPropagation();
        this.beginEdit(span);
      });

      // The browser renders a different face than the PDF used, so the run is
      // stretched back to the width the PDF actually occupied. The glyphs are
      // invisible, so this only affects selection geometry.
      const transforms = [];
      if (Math.abs(run.a) > 0.0005) {
        transforms.push(`rotate(${run.a}rad)`);
      }
      const natural = measureTextWidth(text, size);
      if (natural > 0 && run.w > 0) {
        transforms.push(`scaleX(${run.w / natural})`);
      }
      if (transforms.length > 0) {
        span.style.transform = transforms.join(" ");
      }

      fragment.append(span);
    }

    layer.replaceChildren(fragment);

    // Geometry: lines, table rules, shapes and images, each as a selectable box.
    const objectLayer = this.elements[index]?.objectLayer;
    if (objectLayer) {
      const objectFragment = document.createDocumentFragment();
      const objects = Array.isArray(message.data.objects) ? message.data.objects : [];
      for (const object of objects) {
        const box = document.createElement("div");
        box.className = "object-box";
        box.style.left = `${object.x0}px`;
        box.style.top = `${object.y0}px`;
        box.style.width = `${Math.max(1, object.x1 - object.x0)}px`;
        box.style.height = `${Math.max(1, object.y1 - object.y0)}px`;
        box.dataset.page = String(index);
        box.dataset.x0 = String(object.x0);
        box.dataset.y0 = String(object.y0);
        box.dataset.x1 = String(object.x1);
        box.dataset.y1 = String(object.y1);
        box.dataset.kind = String(object.k);
        box.addEventListener("click", (event) => {
          event.stopPropagation();
          this.selectObject(box);
        });
        objectFragment.append(box);
      }
      objectLayer.replaceChildren(objectFragment);
    }
  }

  // ---- objects (geometry) ---------------------------------------------------

  selectObject(box) {
    if (this.selectedObject) {
      this.selectedObject.classList.remove("object-box--selected");
    }
    this.selectedObject = box;
    box.classList.add("object-box--selected");
  }

  clearSelection() {
    if (this.selectedObject) {
      this.selectedObject.classList.remove("object-box--selected");
    }
    this.selectedObject = null;
  }

  deleteSelection() {
    const box = this.selectedObject;
    if (!box) {
      return;
    }
    this.clearSelection();
    this.onErase?.({
      page: Number(box.dataset.page),
      x0: Number(box.dataset.x0),
      y0: Number(box.dataset.y0),
      x1: Number(box.dataset.x1),
      y1: Number(box.dataset.y1),
    });
  }

  // Switches the active tool. Double-click editing always stays available.
  setTool(tool) {
    this.tool = tool || "select";
    this.pagesEl.classList.toggle("is-editing", this.tool === "edit");
    this.pagesEl.classList.toggle("is-adding", this.tool === "addtext");
    this.pagesEl.classList.toggle("is-erasing", this.tool === "erase");
    this.pagesEl.classList.toggle("is-objects", this.tool === "objects");
    this.cancelErase();
    this.clearSelection();
  }

  installToolHandlers() {
    // Add text: a click on a page places new text at that point.
    this.viewport.addEventListener("click", (event) => {
      if (this.tool !== "addtext") {
        return;
      }
      const hit = this.pointToPdf(event);
      if (hit) {
        this.onInsertText?.(hit);
      }
    });

    // Erase drags a delete box; Edit drags a text-selection box.
    this.viewport.addEventListener("mousedown", (event) => {
      if (event.button !== 0) {
        return;
      }
      if (this.tool === "erase") {
        const hit = this.pointToPdf(event);
        if (!hit) {
          return;
        }
        event.preventDefault();
        this.eraseStart = hit;
        this.showMarquee(event);
      } else if (this.tool === "edit") {
        const hit = this.pointToPdf(event);
        if (!hit) {
          return;
        }
        event.preventDefault();
        this.selectStart = { x: event.clientX, y: event.clientY, page: hit.page };
        this.showMarquee(event);
      }
    });

    window.addEventListener("mousemove", (event) => {
      if (this.eraseStart || this.selectStart) {
        this.updateMarquee(event);
      }
    });

    window.addEventListener("mouseup", (event) => {
      if (!this.eraseStart && !this.selectStart) {
        return;
      }
      const box = {
        left: Math.min(this.marqueeOrigin ? this.marqueeOrigin.x : event.clientX, event.clientX),
        top: Math.min(this.marqueeOrigin ? this.marqueeOrigin.y : event.clientY, event.clientY),
        right: Math.max(this.marqueeOrigin ? this.marqueeOrigin.x : event.clientX, event.clientX),
        bottom: Math.max(this.marqueeOrigin ? this.marqueeOrigin.y : event.clientY, event.clientY),
      };
      const draggedX = box.right - box.left;
      const draggedY = box.bottom - box.top;

      if (this.eraseStart) {
        const start = this.eraseStart;
        const end = this.pointToPdf(event);
        this.eraseStart = null;
        this.hideMarquee();
        if (end && end.page === start.page &&
            (Math.abs(end.x - start.x) > 1 || Math.abs(end.y - start.y) > 1)) {
          this.onErase?.({ page: start.page, x0: start.x, y0: start.y, x1: end.x, y1: end.y });
        }
        return;
      }

      const start = this.selectStart;
      this.selectStart = null;
      this.hideMarquee();
      if (Math.max(draggedX, draggedY) > 4) {
        this.openBlockFor(box, start.page);
      }
    });

    // Delete removes whatever object (line / shape / image) is selected.
    window.addEventListener("keydown", (event) => {
      if (!this.selectedObject) {
        return;
      }
      if (document.activeElement && document.activeElement.isContentEditable) {
        return;
      }
      if (event.key === "Delete" || event.key === "Backspace") {
        event.preventDefault();
        this.deleteSelection();
      } else if (event.key === "Escape") {
        this.clearSelection();
      }
    });
  }

  // Gathers every run the selection box touches on one page, groups them into
  // lines, and opens the block editor over the selection.
  openBlockFor(selRect, pageIndex) {
    const element = this.elements[pageIndex];
    if (!element) {
      return;
    }
    const pageRect = element.wrapper.getBoundingClientRect();
    const spans = [...element.textLayer.querySelectorAll(".text-run")].filter((span) => {
      const r = span.getBoundingClientRect();
      return (
        r.right >= selRect.left &&
        r.left <= selRect.right &&
        r.bottom >= selRect.top &&
        r.top <= selRect.bottom
      );
    });
    if (spans.length === 0) {
      return;
    }

    const runs = [];
    let ux0 = Infinity;
    let uy0 = Infinity;
    let ux1 = -Infinity;
    let uy1 = -Infinity;
    for (const span of spans) {
      const x = Number(span.dataset.x);
      const y = Number(span.dataset.y);
      const w = Number(span.dataset.w);
      const size = Number(span.dataset.s);
      const asc = Number(span.dataset.asc);
      ux0 = Math.min(ux0, x);
      uy0 = Math.min(uy0, y - asc * size);
      ux1 = Math.max(ux1, x + w);
      uy1 = Math.max(uy1, y + 0.3 * size);
      runs.push({
        x,
        y,
        w,
        size,
        c: parseColor(span.dataset.c),
        fid: span.dataset.fid || "",
        text: span.dataset.original ?? span.textContent,
      });
    }

    runs.sort((a, b) => a.y - b.y || a.x - b.x);
    const lines = [];
    for (const run of runs) {
      const tolerance = Math.max(1.5, run.size * 0.3);
      let line = lines.find((candidate) => Math.abs(candidate.y - run.y) < tolerance);
      if (!line) {
        line = { x: run.x, y: run.y, size: run.size, parts: [] };
        lines.push(line);
      } else {
        line.x = Math.min(line.x, run.x);
        line.size = Math.max(line.size, run.size);
      }
      line.parts.push(run);
    }
    lines.sort((a, b) => a.y - b.y);

    const text = lines
      .map((line) => {
        line.parts.sort((a, b) => a.x - b.x);
        let value = "";
        let previousEnd = null;
        for (const part of line.parts) {
          if (previousEnd !== null && part.x - previousEnd > Math.max(2, part.size * 0.25)) {
            value += " ";
          }
          value += part.text;
          previousEnd = part.x + part.w;
        }
        return value;
      })
      .join("\n");

    const block = {
      page: pageIndex,
      x0: ux0,
      y0: uy0,
      x1: ux1,
      y1: uy1,
      size: lines.length > 0 ? lines[0].size : 12,
      c: runs.length > 0 ? runs[0].c : [0, 0, 0],
      fid: runs.length > 0 ? runs[0].fid : "",
      lines: lines.map((line) => ({ x: line.x, y: line.y, s: line.size })),
      text,
      anchor: { left: selRect.left, top: selRect.top, width: selRect.right - selRect.left },
    };
    this.openBlockEditor(block);
  }

  // In-place paragraph editor. Not a floating box: a borderless overlay filled
  // with the page's own colour, sitting exactly on the block in the document's
  // own font, size and colour, so it reads as the page itself. Plain Enter adds a
  // line, Ctrl+Enter (or clicking away) applies, Escape cancels; the toolbar
  // spacing buttons (or Alt+Up/Down) change the leading live.
  openBlockEditor(block) {
    const anchor = block.anchor;
    delete block.anchor;

    const element = this.elements[block.page];
    const lines = block.lines || [];
    const size = lines.length > 0 && lines[0].s > 0 ? lines[0].s : block.size || 12;
    const baseLeading = lines.length >= 2 ? Math.abs(lines[1].y - lines[0].y) : size * 1.35;

    const editor = document.createElement("div");
    editor.className = "block-editor";
    editor.contentEditable = "true";
    editor.spellcheck = false;
    editor.textContent = block.text;
    editor.style.left = `${anchor.left}px`;
    editor.style.top = `${anchor.top}px`;
    editor.style.minWidth = `${Math.max(120, anchor.width)}px`;
    editor.style.fontSize = `${size}px`;
    editor.style.background = this.pageBackgroundColor(element);
    const family = block.fid ? this.onResolveFont?.(block.fid) : "";
    if (family) {
      editor.style.fontFamily = family;
    }
    if (Array.isArray(block.c)) {
      editor.style.color = `rgb(${Math.round(block.c[0] * 255)}, ${Math.round(block.c[1] * 255)}, ${Math.round(block.c[2] * 255)})`;
    }

    let spacing = this.initialSpacing || 1;
    const applySpacing = () => {
      editor.style.lineHeight = `${(baseLeading * spacing).toFixed(2)}px`;
    };
    applySpacing();

    document.body.append(editor);
    editor.focus();

    // Driven by the toolbar while the editor is open.
    this.adjustSpacing = (delta) => {
      spacing = Math.min(3, Math.max(0.5, Math.round((spacing + delta) * 10) / 10));
      applySpacing();
      return spacing;
    };
    this.setSpacing = (value) => {
      spacing = Math.min(3, Math.max(0.5, value));
      applySpacing();
    };

    const selection = window.getSelection();
    const range = document.createRange();
    range.selectNodeContents(editor);
    selection.removeAllRanges();
    selection.addRange(range);

    let done = false;
    const finish = (commit) => {
      if (done) {
        return;
      }
      done = true;
      editor.removeEventListener("keydown", onKey);
      editor.removeEventListener("blur", onBlur);
      const value = editor.innerText.replace(/\r/g, "").replace(/\n$/, "");
      editor.remove();
      this.adjustSpacing = null;
      this.setSpacing = null;
      if (!commit || value === "" || (value === block.text && spacing === 1)) {
        return;
      }

      const firstX = lines.length > 0 ? lines[0].x : block.x0;
      const firstY = lines.length > 0 ? lines[0].y : block.y0 + size;

      // Reflow fits a run of same-size lines (a real paragraph). Mixed sizes - a
      // heading over body text - keep their own lines instead.
      const sizes = lines.map((line) => line.s).filter((lineSize) => lineSize > 0);
      const uniform = sizes.length === 0 || Math.max(...sizes) - Math.min(...sizes) < 1.5;

      if (uniform) {
        const familyName = this.onResolveFont?.(block.fid) || "sans-serif";
        const width = Math.max(40, block.x1 - block.x0);
        const wrapped = wrapText(value, size, familyName, width);
        const leading = baseLeading * spacing;
        const reflowed = wrapped.map((_, index) => ({
          x: firstX,
          y: firstY + index * leading,
          s: size,
        }));
        this.onEditBlock?.({ ...block, spacing, lines: reflowed, text: wrapped.join("\n") });
      } else {
        this.onEditBlock?.({ ...block, spacing, text: value });
      }
    };
    const onKey = (event) => {
      if (event.altKey && (event.key === "ArrowUp" || event.key === "ArrowDown")) {
        event.preventDefault();
        this.adjustSpacing?.(event.key === "ArrowUp" ? 0.1 : -0.1);
      } else if (event.key === "Escape") {
        event.preventDefault();
        finish(false);
      } else if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
        event.preventDefault();
        finish(true);
      }
    };
    const onBlur = () => finish(true);
    editor.addEventListener("keydown", onKey);
    editor.addEventListener("blur", onBlur);
  }

  // Maps a mouse event to { page, x, y } in PDF points (top-left origin).
  pointToPdf(event) {
    for (let index = 0; index < this.elements.length; index += 1) {
      const rect = this.elements[index].wrapper.getBoundingClientRect();
      if (
        event.clientX >= rect.left &&
        event.clientX <= rect.right &&
        event.clientY >= rect.top &&
        event.clientY <= rect.bottom
      ) {
        return {
          page: index,
          x: (event.clientX - rect.left) / this.zoom,
          y: (event.clientY - rect.top) / this.zoom,
        };
      }
    }
    return null;
  }

  showMarquee(event) {
    if (!this.marquee) {
      this.marquee = document.createElement("div");
      this.marquee.className = "marquee";
      document.body.append(this.marquee);
      this.marqueeOrigin = { x: event.clientX, y: event.clientY };
    }
    this.updateMarquee(event);
  }

  updateMarquee(event) {
    if (!this.marquee) {
      return;
    }
    const origin = this.marqueeOrigin;
    this.marquee.style.left = `${Math.min(origin.x, event.clientX)}px`;
    this.marquee.style.top = `${Math.min(origin.y, event.clientY)}px`;
    this.marquee.style.width = `${Math.abs(event.clientX - origin.x)}px`;
    this.marquee.style.height = `${Math.abs(event.clientY - origin.y)}px`;
  }

  hideMarquee() {
    if (this.marquee) {
      this.marquee.remove();
      this.marquee = null;
    }
    this.marqueeOrigin = null;
  }

  cancelErase() {
    if (this.eraseStart || this.selectStart) {
      this.eraseStart = null;
      this.selectStart = null;
      this.hideMarquee();
    }
  }

  // The page's background colour, sampled from the raster so an edit can cover
  // the original glyphs with the page colour instead of a hard-coded white.
  pageBackgroundColor(element) {
    try {
      if (element && element.context && element.canvas.width > 8 && element.canvas.height > 8) {
        const data = element.context.getImageData(4, 4, 1, 1).data;
        return `rgb(${data[0]}, ${data[1]}, ${data[2]})`;
      }
    } catch (error) {
      /* canvas may be empty or (in principle) cross-origin tainted */
    }
    return "#ffffff";
  }

  // Turns a run into an editable box. The glyphs are normally transparent, so
  // the editing state paints them in for the duration. Enter commits, Escape
  // reverts, clicking away commits.
  beginEdit(span) {
    if (span.isContentEditable) {
      return;
    }
    const original = span.dataset.original ?? span.textContent;

    // Make the run look like the document itself while it is edited: the same
    // colour and face, over an opaque box that hides the old raster glyphs. This
    // is what stops an edit looking like a box pasted on top.
    const color = parseColor(span.dataset.c);
    const previousStyle = {
      color: span.style.color,
      background: span.style.background,
      fontFamily: span.style.fontFamily,
    };
    span.style.color = `rgb(${Math.round(color[0] * 255)}, ${Math.round(color[1] * 255)}, ${Math.round(color[2] * 255)})`;
    // Cover the old glyphs with the page's own background colour, sampled from
    // the raster, so the edit sits on the real page rather than a white box.
    span.style.background = this.pageBackgroundColor(this.elements[Number(span.dataset.page)]);
    const family = this.onResolveFont?.(span.dataset.fid);
    if (family) {
      span.style.fontFamily = family;
    }

    span.contentEditable = "true";
    span.classList.add("text-run--editing");
    span.focus();

    const selection = window.getSelection();
    const range = document.createRange();
    range.selectNodeContents(span);
    selection.removeAllRanges();
    selection.addRange(range);

    let done = false;
    const finish = (commit) => {
      if (done) {
        return;
      }
      done = true;
      span.removeEventListener("keydown", onKey);
      span.removeEventListener("blur", onBlur);
      span.contentEditable = "false";
      span.classList.remove("text-run--editing");
      span.style.color = previousStyle.color;
      span.style.background = previousStyle.background;
      span.style.fontFamily = previousStyle.fontFamily;

      const next = span.textContent;
      if (!commit || next === original) {
        span.textContent = original;
        return;
      }
      this.onEditRun?.({
        page: Number(span.dataset.page),
        x: Number(span.dataset.x),
        y: Number(span.dataset.y),
        w: Number(span.dataset.w),
        s: Number(span.dataset.s),
        a: Number(span.dataset.a),
        asc: Number(span.dataset.asc),
        c: parseColor(span.dataset.c),
        fid: span.dataset.fid || "",
        text: next,
      });
    };
    const onKey = (event) => {
      if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        span.blur();
      } else if (event.key === "Escape") {
        event.preventDefault();
        finish(false);
        span.blur();
      }
    };
    const onBlur = () => finish(true);
    span.addEventListener("keydown", onKey);
    span.addEventListener("blur", onBlur);
  }

  // Drops a page's pixels and text and asks for them again. Used after an edit:
  // the engine has rewritten that page's content stream, so both the raster and
  // the selectable layer have to be rebuilt.
  refreshPage(index) {
    if (index == null || !this.elements[index]) {
      return;
    }
    this.releasePixels(index);
    this.lastRequest = null;
    this.update();
  }

  // --------------------------------------------------------------------- zoom

  setZoom(zoom, { preserveAnchor = true } = {}) {
    const clamped = Math.min(Math.max(zoom, ZOOM_MIN), ZOOM_MAX);
    if (Math.abs(clamped - this.zoom) < 1e-4 || !this.hasDocument) {
      return;
    }

    const anchor = preserveAnchor ? this.captureAnchor() : null;
    this.zoom = clamped;
    this.applyLayout();
    this.measure();
    if (anchor) {
      this.restoreAnchor(anchor);
    }

    this.lastRequest = null;
    this.update();
    this.onZoomChange?.(this.zoom);
  }

  zoomIn() {
    this.setZoom(this.zoom + ZOOM_STEP);
  }

  zoomOut() {
    this.setZoom(this.zoom - ZOOM_STEP);
  }

  fitWidth() {
    if (!this.hasDocument || this.pageSizes.length === 0) {
      return;
    }
    const styles = getComputedStyle(this.pagesEl);
    const padding = parseFloat(styles.paddingLeft) + parseFloat(styles.paddingRight);
    const scrollbar = this.viewport.offsetWidth - this.viewport.clientWidth;
    const available = this.viewport.clientWidth - padding - scrollbar - 16;

    let widest = 0;
    for (const size of this.pageSizes) {
      widest = Math.max(widest, size.width);
    }
    if (widest <= 0 || available <= 0) {
      return;
    }
    this.setZoom(available / widest, { preserveAnchor: false });
  }

  captureAnchor() {
    const scrollTop = this.viewport.scrollTop;
    for (let index = 0; index < this.offsets.length; index += 1) {
      const top = this.offsets[index];
      const bottom = top + this.heights[index];
      if (bottom > scrollTop) {
        return { page: index, fraction: (scrollTop - top) / Math.max(1, this.heights[index]) };
      }
    }
    return { page: this.offsets.length - 1, fraction: 0 };
  }

  restoreAnchor(anchor) {
    const top = this.offsets[anchor.page];
    if (top === undefined) {
      return;
    }
    this.viewport.scrollTop = top + this.heights[anchor.page] * anchor.fraction;
  }

  scrollToPage(index) {
    const top = this.offsets[index];
    if (top === undefined) {
      return;
    }
    this.viewport.scrollTo({ top, behavior: "smooth" });
  }
}
