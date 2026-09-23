#include <windows.h>

#include <shellapi.h>

#include <exception>
#include <string>

#include "app/App.h"
#include "util/Logger.h"

namespace {

// Accepts an optional PDF path so Rpfg can be used from the shell or as a
// "Open with" target: Rpfg.exe "C:\docs\manual.pdf"
std::wstring startupFileFromCommandLine() {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return {};
    }
    std::wstring startupFile;
    if (argumentCount > 1) {
        startupFile = arguments[1];
    }
    LocalFree(arguments);
    return startupFile;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previousInstance*/, PWSTR /*commandLine*/,
                    int /*showCommand*/) {
    // Per-monitor v2 keeps both the native window and the WebView crisp when the
    // window is dragged between displays with different scaling.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialised = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"Failed to initialise COM.", L"Rpfg", MB_ICONERROR);
        return 1;
    }

    int exitCode = 0;
    try {
        rpfg::logInfo("Rpfg starting");
        rpfg::App app;
        if (!app.create(instance, startupFileFromCommandLine())) {
            MessageBoxW(nullptr, L"Could not create the main window.", L"Rpfg", MB_ICONERROR);
            exitCode = 1;
        } else {
            exitCode = app.runMessageLoop();
        }
        rpfg::logInfo("Rpfg exited with code " + std::to_string(exitCode));
    } catch (const std::exception& ex) {
        rpfg::logError(std::string("fatal: ") + ex.what());
        MessageBoxA(nullptr, ex.what(), "Rpfg - fatal error", MB_ICONERROR);
        exitCode = 1;
    } catch (...) {
        rpfg::logError("fatal: unknown exception");
        exitCode = 1;
    }

    if (comInitialised) {
        CoUninitialize();
    }
    return exitCode;
}
