// Thin wrapper around the WebView2 host message channel.
//
// Host -> UI messages arrive as already-parsed objects because the C++ side
// posts them with PostWebMessageAsJson. UI -> host messages are plain objects
// handed to postMessage; the host parses them with nlohmann/json.

const handlers = new Map();

export const available = Boolean(window.chrome && window.chrome.webview);

export function on(type, handler) {
  if (!handlers.has(type)) {
    handlers.set(type, new Set());
  }
  handlers.get(type).add(handler);
}

export function send(payload) {
  if (!available) {
    console.warn("[bridge] host unavailable, dropping", payload);
    return;
  }
  window.chrome.webview.postMessage(payload);
}

function dispatch(event) {
  const message = event.data;
  if (!message || typeof message !== "object") {
    return;
  }
  const registered = handlers.get(message.type);
  if (registered) {
    for (const handler of registered) {
      try {
        handler(message);
      } catch (error) {
        console.error(`[bridge] handler for '${message.type}' threw`, error);
      }
    }
  }
}

export function start() {
  if (!available) {
    document.body.dataset.bridge = "missing";
    return false;
  }
  window.chrome.webview.addEventListener("message", dispatch);
  // Tell the host we are listening. The host queues any file it was launched
  // with until this handshake completes.
  send({ cmd: "ready" });
  return true;
}
