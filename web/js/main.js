import * as bridge from "./bridge.js";
import { Viewer } from "./viewer.js";

const elements = {
  openButton: document.getElementById("open-button"),
  emptyOpen: document.getElementById("empty-open"),
  emptyState: document.getElementById("empty-state"),
  fileName: document.getElementById("file-name"),
  zoomIn: document.getElementById("zoom-in"),
  zoomOut: document.getElementById("zoom-out"),
  zoomFit: document.getElementById("zoom-fit"),
  zoomLevel: document.getElementById("zoom-level"),
  pageIndicator: document.getElementById("page-indicator"),
  fontPicker: document.getElementById("font-picker"),
  fontSize: document.getElementById("font-size"),
  textColor: document.getElementById("text-color"),
  spacingDown: document.getElementById("spacing-down"),
  spacingUp: document.getElementById("spacing-up"),
  spacingLevel: document.getElementById("spacing-level"),
  editButton: document.getElementById("edit-button"),
  objectsButton: document.getElementById("objects-button"),
  addTextButton: document.getElementById("add-text-button"),
  eraseButton: document.getElementById("erase-button"),
  saveButton: document.getElementById("save-button"),
  saveAsButton: document.getElementById("save-as-button"),
  dirtyDot: document.getElementById("dirty-dot"),
  statsToggle: document.getElementById("stats-toggle"),
  statsPanel: document.getElementById("stats-panel"),
  toast: document.getElementById("toast"),
  viewport: document.getElementById("viewport"),
  pages: document.getElementById("pages"),
  rail: document.getElementById("page-rail"),
};

const viewer = new Viewer({
  viewport: elements.viewport,
  pages: elements.pages,
  rail: elements.rail,
  onZoomChange: (zoom) => {
    elements.zoomLevel.textContent = `${Math.round(zoom * 100)}%`;
  },
  onPageChange: (index, total) => {
    elements.pageIndicator.textContent = `${index + 1} / ${total}`;
  },
  onEditRun: (edit) => bridge.send({ cmd: "editText", ...withStyle(edit) }),
  onEditBlock: (block) => bridge.send({ cmd: "editBlock", ...withStyleBlock(block) }),
  onInsertText: (hit) => {
    const text = window.prompt("Text to add:");
    if (text === null || text === "") {
      return;
    }
    bridge.send({ cmd: "editText", ...withStyle(hit), replace: false, text });
  },
  onErase: (rect) => bridge.send({ cmd: "eraseRegion", ...rect }),
  onResolveFont: (fid) => fontFamilies.get(fid) || "",
});

// The font/size/colour controls override the run's own look when the user has
// touched them; otherwise an edit matches the document.
let sizeTouched = false;
let fontTouched = false;
let colorTouched = false;
elements.fontSize.addEventListener("input", () => {
  sizeTouched = true;
});
elements.fontPicker.addEventListener("change", () => {
  fontTouched = true;
});
elements.textColor.addEventListener("input", () => {
  colorTouched = true;
});

// Loaded faces, so an edited run can be shown in the document's own font.
const fontFamilies = new Map();
const fontStyleElement = document.createElement("style");
document.head.append(fontStyleElement);

function standardFamily(id) {
  if (id.startsWith("times")) {
    return `"Times New Roman", Times, serif`;
  }
  if (id.startsWith("cour")) {
    return `"Courier New", Courier, monospace`;
  }
  if (id === "symbol") {
    return `Symbol, serif`;
  }
  if (id === "zapf") {
    return `ZapfDingbats, serif`;
  }
  return `Helvetica, Arial, sans-serif`;
}

function registerFonts(fonts) {
  fontFamilies.clear();
  let css = "";
  for (const font of fonts || []) {
    if (font.embedded && font.url) {
      const family = `rpfg-${font.id}`;
      css += `@font-face{font-family:"${family}";src:url("${font.url}");font-display:swap;}\n`;
      fontFamilies.set(font.id, `"${family}"`);
    } else {
      fontFamilies.set(font.id, standardFamily(font.id));
    }
  }
  fontStyleElement.textContent = css;
}

function currentColor() {
  const hex = elements.textColor.value || "#000000";
  return {
    r: parseInt(hex.slice(1, 3), 16) / 255,
    g: parseInt(hex.slice(3, 5), 16) / 255,
    b: parseInt(hex.slice(5, 7), 16) / 255,
  };
}

function withStyle(target) {
  return { ...target, replace: true, ...styleForRun(target) };
}

// An edit reuses the run's own colour and face unless the user changed them, so
// the result matches the document instead of looking pasted on.
function styleForRun(run) {
  const color =
    !colorTouched && Array.isArray(run.c)
      ? { r: Number(run.c[0]) || 0, g: Number(run.c[1]) || 0, b: Number(run.c[2]) || 0 }
      : currentColor();
  const font = !fontTouched && run.fid ? run.fid : elements.fontPicker.value;
  const size = sizeTouched
    ? Number(elements.fontSize.value)
    : Number(run.s) || Number(elements.fontSize.value) || 14;
  return { font, s: size, r: color.r, g: color.g, b: color.b };
}

// A block keeps its own per-line sizes (so the paragraph's spacing survives)
// unless the user has explicitly set a size, which then overrides every line.
function withStyleBlock(block) {
  const style = styleForRun({ c: block.c, fid: block.fid, s: block.size });
  const lines = sizeTouched
    ? (block.lines || []).map((line) => ({ ...line, s: style.s }))
    : block.lines;
  return { ...block, lines, ...style };
}

let toastTimer = 0;
let statsTimer = 0;

function showToast(message) {
  elements.toast.textContent = message;
  elements.toast.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => {
    elements.toast.hidden = true;
  }, 6000);
}

function setDocumentControlsEnabled(enabled) {
  elements.zoomIn.disabled = !enabled;
  elements.zoomOut.disabled = !enabled;
  elements.zoomFit.disabled = !enabled;
  elements.statsToggle.disabled = !enabled;
  elements.saveButton.disabled = !enabled;
  elements.saveAsButton.disabled = !enabled;
  elements.editButton.disabled = !enabled;
  elements.objectsButton.disabled = !enabled;
  elements.addTextButton.disabled = !enabled;
  elements.eraseButton.disabled = !enabled;
  elements.fontPicker.disabled = !enabled;
  elements.fontSize.disabled = !enabled;
  elements.textColor.disabled = !enabled;
  elements.spacingDown.disabled = !enabled;
  elements.spacingUp.disabled = !enabled;
  if (!enabled) {
    setTool("select");
  }
}

const toolButtons = {
  edit: elements.editButton,
  objects: elements.objectsButton,
  addtext: elements.addTextButton,
  erase: elements.eraseButton,
};

function setTool(tool) {
  for (const [name, button] of Object.entries(toolButtons)) {
    button.setAttribute("aria-pressed", name === tool ? "true" : "false");
  }
  viewer.setTool(tool);
  if (tool === "edit") {
    showToast("Edit mode — click text to change it, or drag to edit a paragraph");
  } else if (tool === "objects") {
    showToast("Objects — click a line, rule, shape or image to select it, then Delete");
  } else if (tool === "addtext") {
    showToast("Add text — click on the page to place it");
  } else if (tool === "erase") {
    showToast("Erase — drag a box over shapes, lines, images or text to delete them");
  }
}

for (const [name, button] of Object.entries(toolButtons)) {
  button.addEventListener("click", () => {
    const active = button.getAttribute("aria-pressed") === "true";
    setTool(active ? "select" : name);
  });
}

function populateFonts(fonts) {
  elements.fontPicker.replaceChildren();
  for (const font of fonts || []) {
    const option = document.createElement("option");
    option.value = font.id;
    option.textContent = font.label;
    elements.fontPicker.append(option);
  }
  const options = [...elements.fontPicker.options];
  const preferred =
    options.find((option) => option.value === "times-new-roman") ||
    options.find((option) => option.value === "times");
  if (preferred) {
    elements.fontPicker.value = preferred.value;
  }
}

function setDirty(dirty) {
  elements.dirtyDot.hidden = !dirty;
}

function describeBytes(bytes) {
  if (bytes < 1024) {
    return `${bytes} B`;
  }
  if (bytes < 1024 * 1024) {
    return `${(bytes / 1024).toFixed(1)} KB`;
  }
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function renderStats(stats) {
  const rows = [
    ["worker threads", stats.threads],
    ["queued", stats.queued],
    ["jobs submitted", stats.submitted],
    ["jobs coalesced", stats.coalesced],
    ["jobs cancelled", stats.cancelled],
    ["pages rasterised", stats.rasterised],
    ["display-list hits", stats.displayListHits],
    ["bitmap-cache hits", stats.cacheHits],
    ["cached rasters", stats.cacheEntries],
    ["cache size", describeBytes(stats.cacheBytes)],
    ["text layers", stats.textLayers],
    ["text cached", stats.textCacheEntries],
    ["edits applied", stats.edits],
  ];
  elements.statsPanel.textContent = rows
    .map(([label, value]) => `${label.padEnd(18)}${String(value).padStart(9)}`)
    .join("\n");
}

// -------------------------------------------------------------------- wiring

elements.openButton.addEventListener("click", () => bridge.send({ cmd: "openDialog" }));
elements.emptyOpen.addEventListener("click", () => bridge.send({ cmd: "openDialog" }));

elements.zoomIn.addEventListener("click", () => viewer.zoomIn());
elements.zoomOut.addEventListener("click", () => viewer.zoomOut());
elements.zoomFit.addEventListener("click", () => viewer.fitWidth());

elements.saveButton.addEventListener("click", () => bridge.send({ cmd: "save" }));
elements.saveAsButton.addEventListener("click", () => bridge.send({ cmd: "saveAs" }));

// Line spacing up/down. mousedown is swallowed so pressing a button does not
// blur (and therefore commit) the paragraph being edited.
let lineSpacing = 1;
function setLineSpacing(value) {
  lineSpacing = Math.min(3, Math.max(0.5, Math.round(value * 10) / 10));
  elements.spacingLevel.textContent = `${lineSpacing.toFixed(1)}×`;
  viewer.initialSpacing = lineSpacing;
  viewer.setSpacing?.(lineSpacing);
}
for (const [button, delta] of [
  [elements.spacingDown, -0.1],
  [elements.spacingUp, 0.1],
]) {
  button.addEventListener("mousedown", (event) => event.preventDefault());
  button.addEventListener("click", () => setLineSpacing(lineSpacing + delta));
}

elements.statsToggle.addEventListener("change", () => {
  const enabled = elements.statsToggle.checked;
  elements.statsPanel.hidden = !enabled;
  clearInterval(statsTimer);
  if (enabled) {
    bridge.send({ cmd: "stats" });
    statsTimer = window.setInterval(() => bridge.send({ cmd: "stats" }), 700);
  }
});

window.addEventListener("keydown", (event) => {
  // Ctrl+S works even while a run is being edited.
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
    event.preventDefault();
    bridge.send({ cmd: event.shiftKey ? "saveAs" : "save" });
    return;
  }
  // While editing text, let the browser handle every other key.
  if (document.activeElement && document.activeElement.isContentEditable) {
    return;
  }
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "o") {
    event.preventDefault();
    bridge.send({ cmd: "openDialog" });
    return;
  }
  if (event.ctrlKey && (event.key === "=" || event.key === "+")) {
    event.preventDefault();
    viewer.zoomIn();
    return;
  }
  if (event.ctrlKey && event.key === "-") {
    event.preventDefault();
    viewer.zoomOut();
    return;
  }
  if (event.ctrlKey && event.key === "0") {
    event.preventDefault();
    viewer.fitWidth();
    return;
  }
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
    // Scope select-all to the document, not the whole chrome.
    const selection = window.getSelection();
    if (selection) {
      event.preventDefault();
      selection.removeAllRanges();
      const range = document.createRange();
      range.selectNodeContents(elements.pages);
      selection.addRange(range);
    }
  }
});

elements.viewport.addEventListener(
  "wheel",
  (event) => {
    if (!event.ctrlKey) {
      return;
    }
    event.preventDefault();
    if (event.deltaY < 0) {
      viewer.zoomIn();
    } else {
      viewer.zoomOut();
    }
  },
  { passive: false },
);

// ------------------------------------------------------------ host messages

bridge.on("document", (message) => {
  elements.fileName.textContent = message.title
    ? `${message.title} · ${message.pageCount} pages`
    : `${message.pageCount} pages`;
  elements.fileName.title = message.path;
  elements.emptyState.hidden = true;
  setDocumentControlsEnabled(true);

  viewer.loadDocument(message);
  populateFonts(message.fonts);
  registerFonts(message.fonts);
  setTool("select");
  setDirty(false);
  elements.zoomLevel.textContent = `${Math.round(viewer.zoom * 100)}%`;
  console.info(`[rpfg] rendering with ${message.threads} worker threads`);
  showToast("Double-click text to edit it, or pick a tool from the toolbar");
});

bridge.on("page", (message) => viewer.handleRenderedPage(message));

bridge.on("text", (message) => viewer.handlePageText(message));

bridge.on("edited", (message) => {
  setDirty(true);
  viewer.refreshPage(message.page);
  showToast(`Edited page ${message.page + 1} — press Ctrl+S to save`);
});

bridge.on("saved", (message) => {
  setDirty(false);
  showToast(message.incremental ? "Saved instantly" : `Saved to ${message.path}`);
});

bridge.on("stats", (message) => renderStats(message));

bridge.on("error", (message) => {
  showToast(message.message || "Something went wrong.");
  elements.emptyState.hidden = viewer.hasDocument;
  if (!viewer.hasDocument) {
    elements.fileName.textContent = "No document open";
  }
});

bridge.on("hostReady", () => {
  setDocumentControlsEnabled(false);
});

// ------------------------------------------------------------------- startup

setDocumentControlsEnabled(false);
if (!bridge.start()) {
  showToast("Running outside the Rpfg host — the PDF engine is unavailable.");
}
