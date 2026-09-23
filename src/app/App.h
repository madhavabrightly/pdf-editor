#pragma once

#include <windows.h>

// WebView2.h forward-declares its interfaces with the `interface` macro, which
// the Windows SDK defines in <basetyps.h> - reached through <objbase.h>.
// WIN32_LEAN_AND_MEAN keeps windows.h from pulling the COM headers in, so bring
// them in explicitly, before WebView2.h.
#include <objbase.h>

#include <WebView2.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pdf/PdfEngine.h"

namespace rpfg {

// Custom window messages. Rasterising and text extraction both happen on the
// worker pool, so finished work is handed back to the UI thread through the
// message queue.
constexpr UINT kMsgEngineResults = WM_APP + 1;
constexpr UINT kMsgShowOpenDialog = WM_APP + 2;

// Owns the Win32 window, the WebView2 control and the PDF engine, and shuttles
// JSON messages between the HTML UI and the engine.
class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool create(HINSTANCE instance, const std::wstring& startupFile);
    int runMessageLoop();

private:
    static LRESULT CALLBACK windowProcThunk(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool createMainWindow(HINSTANCE instance);
    void createWebView();
    void onEnvironmentReady(HRESULT result, ICoreWebView2Environment* environment);
    void onControllerReady(HRESULT result, ICoreWebView2Controller* controller);

    void onWebMessage(const std::wstring& json);
    void onEngineResults();
    void onShowOpenDialog();
    void onCloseRequested();

    void postJson(const std::string& json);
    void openDocument(const std::wstring& path);
    void publishDocument(const DocumentInfo& info);
    void publishStats();
    void publishError(const std::string& message);
    void publishSaved(const std::wstring& path, bool incremental);

    // Saves the document to `path` and reports the result to the UI. An empty
    // path means "the document has never been written", which routes to Save As.
    void saveTo(const std::wstring& path, SaveMode mode);
    void updateWindowTitle();

    void resizeWebView();
    std::wstring webRootDirectory() const;

    HWND window_ = nullptr;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller3> controller3_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;

    std::mutex queueMutex_;
    std::vector<std::shared_ptr<const RenderedPage>> renderQueue_;
    std::vector<std::shared_ptr<const PageText>> textQueue_;

    std::wstring webRoot_;
    std::wstring pendingOpenPath_;
    bool webUiReady_ = false;

    // Kept so the window title can be rebuilt (and a dirty marker added) without
    // re-querying the engine.
    DocumentInfo document_;

    // Declared last so it is destroyed FIRST. Its destructor joins the render
    // pool, and workers call back into this object - the callback must be
    // stopped, and the pool drained, before queueMutex_/renderQueue_ go away.
    PdfEngine engine_;
};

}  // namespace rpfg
