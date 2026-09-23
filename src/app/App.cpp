#include "app/App.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
// Microsoft::WRL::Callback is declared in wrl/event.h (implements.h only brings
// in the RuntimeClass machinery).
#include <wrl/event.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "util/Encoding.h"
#include "util/Logger.h"

namespace rpfg {
namespace {

constexpr wchar_t kWindowClass[] = L"RpfgMainWindow";
constexpr wchar_t kWindowTitle[] = L"Rpfg";
constexpr wchar_t kVirtualHost[] = L"app.local";

std::wstring executableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size() - 1) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    const std::size_t slash = buffer.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : buffer.substr(0, slash + 1);
}

std::wstring userDataDirectory() {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    std::wstring base;
    if (_wdupenv_s(&value, &length, L"LOCALAPPDATA") == 0 && value != nullptr) {
        base = value;
        std::free(value);
    } else {
        base = executableDirectory();
    }
    return base + L"\\Rpfg\\WebView2";
}

void ensureDirectory(const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS &&
        result != ERROR_FILE_EXISTS) {
        logError("could not create '" + wideToUtf8(path) + "' (error " + std::to_string(result) +
                 ")");
    }
}

void showWebView2Missing(HWND owner, HRESULT hr) {
    logError("WebView2 environment creation failed: 0x" + std::to_string(static_cast<unsigned>(hr)));
    MessageBoxW(owner,
                L"Rpfg needs the Microsoft Edge WebView2 Runtime.\n\n"
                L"Windows 10 and 11 normally include it. If it is missing, install the "
                L"\"Evergreen Standalone Installer\" from:\n"
                L"https://developer.microsoft.com/microsoft-edge/webview2/",
                L"WebView2 Runtime required", MB_ICONERROR | MB_OK);
}

std::optional<std::wstring> pickPdfFile(HWND owner) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        logError("could not create the file open dialog");
        return std::nullopt;
    }

    const COMDLG_FILTERSPEC filters[] = {
        {L"PDF documents", L"*.pdf"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(L"Open PDF");
    dialog->SetDefaultExtension(L"pdf");

    if (FAILED(dialog->Show(owner))) {
        return std::nullopt;  // includes the user cancelling
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return std::nullopt;
    }

    PWSTR filePath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &filePath)) || filePath == nullptr) {
        return std::nullopt;
    }
    std::wstring result(filePath);
    CoTaskMemFree(filePath);
    return result;
}

std::optional<std::wstring> pickSavePath(HWND owner, const std::wstring& suggestedName,
                                         const std::wstring& title) {
    Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        logError("could not create the file save dialog");
        return std::nullopt;
    }

    const COMDLG_FILTERSPEC filters[] = {
        {L"PDF documents", L"*.pdf"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(title.c_str());
    dialog->SetDefaultExtension(L"pdf");
    if (!suggestedName.empty()) {
        dialog->SetFileName(suggestedName.c_str());
    }

    if (FAILED(dialog->Show(owner))) {
        return std::nullopt;  // includes the user cancelling
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return std::nullopt;
    }

    PWSTR filePath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &filePath)) || filePath == nullptr) {
        return std::nullopt;
    }
    std::wstring result(filePath);
    CoTaskMemFree(filePath);
    return result;
}

std::wstring fileNameOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The virtual host that serves a given font file, so the UI can @font-face it and
// render an edit in the document's own face. Two hosts cover the two places fonts
// come from: the app's `fonts` folder and the Windows font directory.
std::wstring windowsFontsDirectory() {
    std::wstring directory(MAX_PATH, L'\0');
    const UINT length = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0) {
        return {};
    }
    directory.resize(length);
    if (!directory.empty() && directory.back() != L'\\') {
        directory.push_back(L'\\');
    }
    directory += L"Fonts";
    return directory;
}

bool hasPrefixIgnoreCase(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::string fontFileUrl(const std::string& utf8Path) {
    const std::string fileName = std::filesystem::path(utf8Path).filename().string();
    const std::string windowsFonts = wideToUtf8(windowsFontsDirectory());
    const bool inWindows = !windowsFonts.empty() && hasPrefixIgnoreCase(utf8Path, windowsFonts);
    return std::string("https://") + (inWindows ? "winfonts.local/" : "fonts.local/") + fileName;
}

std::wstring documentWindowTitle(const DocumentInfo& info, const std::wstring& path) {
    std::wstring name = path;
    const std::size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        name = name.substr(slash + 1);
    }

    std::wstring title = name.empty() ? L"Untitled" : name;
    if (!info.title.empty()) {
        title += L" \u2014 " + utf8ToWide(info.title);
    }
    title += L" (" + std::to_wstring(info.pageCount) + L" pages) \u2014 Rpfg";
    return title;
}

}  // namespace

App::App() {
    // Both callbacks fire on a worker thread. They only queue their result and
    // wake the UI thread, which is the sole owner of every WebView2 call.
    engine_.setPageReadyCallback([this](std::shared_ptr<const RenderedPage> page) {
        {
            std::lock_guard<std::mutex> guard(queueMutex_);
            renderQueue_.push_back(std::move(page));
        }
        // Rendering off the UI thread is the whole point; wake it up to publish.
        if (window_ != nullptr) {
            PostMessageW(window_, kMsgEngineResults, 0, 0);
        }
    });

    engine_.setPageTextCallback([this](std::shared_ptr<const PageText> text) {
        {
            std::lock_guard<std::mutex> guard(queueMutex_);
            textQueue_.push_back(std::move(text));
        }
        if (window_ != nullptr) {
            PostMessageW(window_, kMsgEngineResults, 0, 0);
        }
    });
}

App::~App() {
    engine_.setPageReadyCallback(nullptr);
    engine_.setPageTextCallback(nullptr);

    // A one-line summary of what the pool actually did; the numbers are the
    // quickest way to tell whether the scheduling behaved (lots of coalescing on
    // fast scrolls, few cancellations when idle, display-list reuse when zooming).
    const EngineStats stats = engine_.stats();
    logInfo("engine totals: " + std::to_string(stats.pagesRasterised) + " rasterised, " +
            std::to_string(stats.jobsSubmitted) + " submitted, " +
            std::to_string(stats.jobsCoalesced) + " coalesced, " +
            std::to_string(stats.jobsCancelled) + " cancelled, " +
            std::to_string(stats.bitmapCacheHits) + " bitmap-cache hits, " +
            std::to_string(stats.displayListHits) + " display-list reuses, " +
            std::to_string(stats.textLayersBuilt) + " text layers");

    engine_.closeDocument();
    if (controller_) {
        controller_->Close();
    }
    controller_.Reset();
    webview_.Reset();
    environment_.Reset();
}

bool App::create(HINSTANCE instance, const std::wstring& startupFile) {
    pendingOpenPath_ = startupFile;

    // Extra faces for the editor live next to the exe; users can drop .ttf/.otf
    // files in there. (Windows' own font directory is scanned by the engine too.)
    engine_.setFontsDirectory(wideToUtf8(executableDirectory() + L"fonts"));

    if (!createMainWindow(instance)) {
        return false;
    }
    createWebView();
    return true;
}

int App::runMessageLoop() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK App::windowProcThunk(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->windowProc(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE:
            resizeWebView();
            return 0;

        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            resizeWebView();
            return 0;
        }

        case WM_DROPFILES: {
            const HDROP drop = reinterpret_cast<HDROP>(wParam);
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            if (length > 0) {
                std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
                DragQueryFileW(drop, 0, path.data(), length + 1);
                path.resize(length);
                DragFinish(drop);
                openDocument(path);
            } else {
                DragFinish(drop);
            }
            return 0;
        }

        case kMsgEngineResults:
            onEngineResults();
            return 0;

        case kMsgShowOpenDialog:
            onShowOpenDialog();
            return 0;

        case WM_CLOSE:
            onCloseRequested();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

bool App::createMainWindow(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &App::windowProcThunk;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;

    if (RegisterClassExW(&windowClass) == 0) {
        logError("RegisterClassExW failed");
        return false;
    }

    constexpr int kWidth = 1440;
    constexpr int kHeight = 920;
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - kWidth) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - kHeight) / 2);

    const HWND window =
        CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, kWidth, kHeight,
                        nullptr, nullptr, instance, this);
    if (window == nullptr) {
        logError("CreateWindowExW failed");
        return false;
    }

    DragAcceptFiles(window, TRUE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return true;
}

void App::createWebView() {
    webRoot_ = webRootDirectory();
    if (!std::filesystem::exists(webRoot_)) {
        logError("web assets are missing at " + wideToUtf8(webRoot_));
        MessageBoxW(window_, L"The 'web' folder is missing next to Rpfg.exe.", L"Rpfg",
                    MB_ICONERROR);
    }

    const std::wstring userData = userDataDirectory();
    ensureDirectory(userData);

    auto handler =
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                onEnvironmentReady(result, environment);
                return S_OK;
            });

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.empty() ? nullptr : userData.c_str(), nullptr, handler.Get());
    if (FAILED(hr)) {
        showWebView2Missing(window_, hr);
    }
}

void App::onEnvironmentReady(HRESULT result, ICoreWebView2Environment* environment) {
    if (FAILED(result) || environment == nullptr) {
        showWebView2Missing(window_, result);
        return;
    }
    environment_ = environment;

    auto handler =
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                onControllerReady(controllerResult, controller);
                return S_OK;
            });

    const HRESULT hr = environment_->CreateCoreWebView2Controller(window_, handler.Get());
    if (FAILED(hr)) {
        showWebView2Missing(window_, hr);
    }
}

void App::onControllerReady(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || controller == nullptr) {
        showWebView2Missing(window_, result);
        return;
    }

    controller_ = controller;
    if (FAILED(controller_->get_CoreWebView2(&webview_)) || !webview_) {
        logError("could not obtain ICoreWebView2");
        return;
    }

    resizeWebView();
    controller_->put_IsVisible(TRUE);

    // RasterizationScale only exists on ICoreWebView2Controller3 (runtime
    // 1.0.864.35+). Without it the WebView still works, just without DPI-matched
    // rasterisation.
    if (FAILED(controller_->QueryInterface(IID_PPV_ARGS(&controller3_)))) {
        controller3_.Reset();
    }

    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings))) {
        settings->put_AreDevToolsEnabled(TRUE);
        settings->put_AreDefaultContextMenusEnabled(TRUE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
    }

    EventRegistrationToken token{};
    const HRESULT handlerResult = webview_->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2* /*sender*/,
                   ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw != nullptr) {
                    std::wstring json(raw);
                    CoTaskMemFree(raw);
                    onWebMessage(json);
                }
                return S_OK;
            })
            .Get(),
        &token);
    if (FAILED(handlerResult)) {
        logError("add_WebMessageReceived failed");
    }

    // Serve the UI from https://app.local/ rather than file:// so it behaves as
    // a proper origin (module imports, fetch, DevTools all work normally).
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview3)))) {
        webview3->SetVirtualHostNameToFolderMapping(kVirtualHost, webRoot_.c_str(),
                                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        // Serve the font folders too, so the UI can load a document's own face
        // with @font-face and edit text in place without it looking pasted on.
        webview3->SetVirtualHostNameToFolderMapping(
            L"fonts.local", (executableDirectory() + L"fonts").c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        const std::wstring windowsFonts = windowsFontsDirectory();
        if (!windowsFonts.empty()) {
            webview3->SetVirtualHostNameToFolderMapping(
                L"winfonts.local", windowsFonts.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }
        webview_->Navigate((std::wstring(L"https://") + kVirtualHost + L"/index.html").c_str());
    } else {
        std::wstring url = webRoot_;
        std::replace(url.begin(), url.end(), L'\\', L'/');
        webview_->Navigate((L"file:///" + url + L"/index.html").c_str());
    }

    logInfo("web view ready (serving " + wideToUtf8(webRoot_) + ")");
}

void App::resizeWebView() {
    if (!controller_) {
        return;
    }
    RECT bounds{};
    GetClientRect(window_, &bounds);
    controller_->put_Bounds(bounds);

    const UINT dpi = GetDpiForWindow(window_);
    if (dpi != 0 && controller3_ != nullptr) {
        controller3_->put_RasterizationScale(static_cast<double>(dpi) / 96.0);
    }
}

std::wstring App::webRootDirectory() const {
    return executableDirectory() + L"web";
}

void App::postJson(const std::string& json) {
    if (!webview_ || json.empty()) {
        return;
    }
    const std::wstring wide = utf8ToWide(json);
    if (wide.empty()) {
        return;
    }
    if (FAILED(webview_->PostWebMessageAsJson(wide.c_str()))) {
        logError("PostWebMessageAsJson failed");
    }
}

void App::onWebMessage(const std::wstring& json) {
    try {
        const nlohmann::json message = nlohmann::json::parse(wideToUtf8(json));
        const std::string command = message.value("cmd", std::string());

        if (command == "ready") {
            webUiReady_ = true;
            postJson("{\"type\":\"hostReady\"}");
            if (!pendingOpenPath_.empty()) {
                const std::wstring path = pendingOpenPath_;
                pendingOpenPath_.clear();
                openDocument(path);
            }
            return;
        }

        if (command == "openDialog") {
            // Show the modal dialog once the message handler has returned.
            PostMessageW(window_, kMsgShowOpenDialog, 0, 0);
            return;
        }

        if (command == "openPath") {
            openDocument(utf8ToWide(message.value("path", std::string())));
            return;
        }

        if (command == "viewport") {
            std::vector<int> pages;
            if (message.contains("pages") && message["pages"].is_array()) {
                pages.reserve(message["pages"].size());
                for (const auto& page : message["pages"]) {
                    pages.push_back(page.get<int>());
                }
            }
            engine_.requestViewport(pages, message.value("scale", 1.0f));
            return;
        }

        if (command == "stats") {
            publishStats();
            return;
        }

        if (command == "save") {
            saveTo(utf8ToWide(engine_.currentPath()), SaveMode::Incremental);
            return;
        }

        if (command == "saveAs") {
            const std::wstring suggestion = document_.path.empty()
                                                ? L"document.pdf"
                                                : fileNameOf(utf8ToWide(engine_.currentPath()));
            const std::optional<std::wstring> path =
                pickSavePath(window_, suggestion, L"Save PDF As");
            if (path.has_value()) {
                saveTo(*path, SaveMode::Full);
            }
            return;
        }

        if (command == "saveCopy") {
            std::wstring suggestion = L"document.pdf";
            if (!engine_.currentPath().empty()) {
                suggestion = L"copy of " + fileNameOf(utf8ToWide(engine_.currentPath()));
            }
            const std::optional<std::wstring> path =
                pickSavePath(window_, suggestion, L"Save a Copy");
            if (path.has_value()) {
                saveTo(*path, SaveMode::Copy);
            }
            return;
        }

        if (command == "editText") {
            TextEdit edit;
            edit.page = message.value("page", 0);
            edit.x = message.value("x", 0.0f);
            edit.y = message.value("y", 0.0f);
            edit.width = message.value("w", 0.0f);
            edit.size = message.value("s", 12.0f);
            edit.angle = message.value("a", 0.0f);
            edit.ascender = message.value("asc", 0.8f);
            edit.r = message.value("r", 0.0f);
            edit.g = message.value("g", 0.0f);
            edit.b = message.value("b", 0.0f);
            edit.replace = message.value("replace", true);
            edit.font = message.value("font", std::string());
            edit.text = message.value("text", std::string());

            std::string error;
            if (!engine_.applyTextEdit(edit, error)) {
                publishError("Edit failed: " + error);
                return;
            }

            nlohmann::json ack;
            ack["type"] = "edited";
            ack["page"] = edit.page;
            postJson(ack.dump());
            updateWindowTitle();
            return;
        }

        if (command == "editBlock") {
            TextBlockEdit block;
            block.page = message.value("page", 0);
            block.x0 = message.value("x0", 0.0f);
            block.y0 = message.value("y0", 0.0f);
            block.x1 = message.value("x1", 0.0f);
            block.y1 = message.value("y1", 0.0f);
            block.size = message.value("s", 12.0f);
            block.spacing = message.value("spacing", 1.0f);
            block.r = message.value("r", 0.0f);
            block.g = message.value("g", 0.0f);
            block.b = message.value("b", 0.0f);
            block.font = message.value("font", std::string());
            block.text = message.value("text", std::string());
            if (message.contains("lines") && message["lines"].is_array()) {
                for (const auto& line : message["lines"]) {
                    TextLineBox box;
                    box.x = line.value("x", 0.0f);
                    box.y = line.value("y", 0.0f);
                    box.size = line.value("s", 0.0f);
                    block.lines.push_back(box);
                }
            }

            std::string error;
            if (!engine_.applyTextBlock(block, error)) {
                publishError("Edit failed: " + error);
                return;
            }

            nlohmann::json ack;
            ack["type"] = "edited";
            ack["page"] = block.page;
            postJson(ack.dump());
            updateWindowTitle();
            return;
        }

        if (command == "eraseRegion") {
            const int page = message.value("page", 0);
            std::string error;
            if (!engine_.eraseRegion(page, message.value("x0", 0.0f), message.value("y0", 0.0f),
                                     message.value("x1", 0.0f), message.value("y1", 0.0f), error)) {
                publishError("Erase failed: " + error);
                return;
            }

            nlohmann::json ack;
            ack["type"] = "edited";
            ack["page"] = page;
            postJson(ack.dump());
            updateWindowTitle();
            return;
        }

        logError("unknown command from UI: " + command);
    } catch (const std::exception& ex) {
        logError(std::string("could not handle UI message: ") + ex.what());
    }
}

void App::onShowOpenDialog() {
    const std::optional<std::wstring> path = pickPdfFile(window_);
    if (path.has_value()) {
        openDocument(*path);
    }
    // Return keyboard focus to the page so Ctrl+O and scrolling keep working.
    if (controller_ != nullptr) {
        controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

void App::openDocument(const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    if (!webUiReady_) {
        pendingOpenPath_ = path;
        return;
    }

    std::string error;
    const std::optional<DocumentInfo> info = engine_.openDocument(wideToUtf8(path), error);
    if (!info.has_value()) {
        publishError(error);
        return;
    }

    SetWindowTextW(window_, documentWindowTitle(*info, path).c_str());
    document_ = *info;
    publishDocument(*info);
}

void App::publishDocument(const DocumentInfo& info) {
    nlohmann::json payload;
    payload["type"] = "document";
    payload["path"] = info.path;
    payload["title"] = info.title;
    payload["pageCount"] = info.pageCount;
    payload["threads"] = engine_.stats().workerThreads;
    payload["pages"] = nlohmann::json::array();
    for (const PageSize& page : info.pages) {
        payload["pages"].push_back({{"width", page.width}, {"height", page.height}});
    }
    payload["fonts"] = nlohmann::json::array();
    for (const FontInfo& font : engine_.availableFonts()) {
        nlohmann::json entry = {
            {"id", font.id}, {"label", font.label}, {"embedded", font.embedded}};
        if (font.embedded && !font.path.empty()) {
            entry["url"] = fontFileUrl(font.path);
        }
        payload["fonts"].push_back(std::move(entry));
    }
    postJson(payload.dump());
}

void App::publishStats() {
    const EngineStats stats = engine_.stats();
    nlohmann::json payload;
    payload["type"] = "stats";
    payload["threads"] = stats.workerThreads;
    payload["queued"] = stats.queuedJobs;
    payload["submitted"] = stats.jobsSubmitted;
    payload["coalesced"] = stats.jobsCoalesced;
    payload["cancelled"] = stats.jobsCancelled;
    payload["rasterised"] = stats.pagesRasterised;
    payload["displayListHits"] = stats.displayListHits;
    payload["cacheHits"] = stats.bitmapCacheHits;
    payload["cacheEntries"] = stats.bitmapCacheEntries;
    payload["cacheBytes"] = stats.bitmapCacheBytes;
    payload["textLayers"] = stats.textLayersBuilt;
    payload["textCacheEntries"] = stats.textCacheEntries;
    payload["edits"] = stats.editsApplied;
    postJson(payload.dump());
}

void App::publishError(const std::string& message) {
    nlohmann::json payload;
    payload["type"] = "error";
    payload["message"] = message;
    postJson(payload.dump());
}

void App::publishSaved(const std::wstring& path, bool incremental) {
    nlohmann::json payload;
    payload["type"] = "saved";
    payload["path"] = wideToUtf8(path);
    payload["incremental"] = incremental;
    postJson(payload.dump());
}

void App::updateWindowTitle() {
    if (!engine_.isOpen()) {
        SetWindowTextW(window_, kWindowTitle);
        return;
    }
    std::wstring title = documentWindowTitle(document_, utf8ToWide(engine_.currentPath()));
    if (engine_.hasUnsavedChanges()) {
        title += L" *";
    }
    SetWindowTextW(window_, title.c_str());
}

void App::saveTo(const std::wstring& path, SaveMode mode) {
    if (!engine_.isOpen()) {
        return;
    }

    std::wstring target = path;
    SaveMode effective = mode;
    if (target.empty()) {
        // The document has never been written anywhere; Save behaves as Save As.
        const std::optional<std::wstring> chosen =
            pickSavePath(window_, L"document.pdf", L"Save PDF As");
        if (!chosen.has_value()) {
            return;
        }
        target = *chosen;
        effective = SaveMode::Full;
    }

    std::string error;
    if (!engine_.saveDocument(wideToUtf8(target), effective, error)) {
        publishError("Save failed: " + error);
        return;
    }

    updateWindowTitle();
    publishSaved(target, effective == SaveMode::Incremental);
    if (controller_ != nullptr) {
        controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

void App::onCloseRequested() {
    if (engine_.hasUnsavedChanges()) {
        const int choice = MessageBoxW(window_,
                                       L"This document has unsaved changes.\n\nSave before "
                                       L"closing?",
                                       L"Rpfg", MB_ICONWARNING | MB_YESNOCANCEL);
        if (choice == IDCANCEL) {
            return;
        }
        if (choice == IDYES) {
            saveTo(utf8ToWide(engine_.currentPath()), SaveMode::Incremental);
            if (engine_.hasUnsavedChanges()) {
                return;  // the save failed or was cancelled; stay open
            }
        }
    }
    DestroyWindow(window_);
}

void App::onEngineResults() {
    std::vector<std::shared_ptr<const RenderedPage>> rasterBatch;
    std::vector<std::shared_ptr<const PageText>> textBatch;
    {
        std::lock_guard<std::mutex> guard(queueMutex_);
        rasterBatch.swap(renderQueue_);
        textBatch.swap(textQueue_);
    }

    for (const std::shared_ptr<const RenderedPage>& page : rasterBatch) {
        if (!page || !page->base64Png) {
            continue;
        }
        // Hand-built JSON: the payload is a multi-megabyte base64 string and a
        // generic serializer would copy it several times over.
        std::string payload;
        payload.reserve(page->base64Png->size() + 160);
        payload += "{\"type\":\"page\",\"page\":";
        payload += std::to_string(page->page);
        payload += ",\"scale\":";
        payload += std::to_string(page->scale);
        payload += ",\"width\":";
        payload += std::to_string(page->pixelWidth);
        payload += ",\"height\":";
        payload += std::to_string(page->pixelHeight);
        payload += ",\"image\":\"data:image/png;base64,";
        payload += *page->base64Png;
        payload += "\"}";
        postJson(payload);
    }

    for (const std::shared_ptr<const PageText>& text : textBatch) {
        if (!text || !text->runs) {
            continue;
        }
        // The run list is already JSON, so splice it in rather than round-trip
        // it through a parser just to re-serialise it.
        std::string payload;
        payload.reserve(text->runs->size() + 48);
        payload += "{\"type\":\"text\",\"page\":";
        payload += std::to_string(text->page);
        payload += ",\"data\":";
        payload += *text->runs;
        payload += "}";
        postJson(payload);
    }
}

}  // namespace rpfg
