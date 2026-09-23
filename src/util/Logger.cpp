#include "util/Logger.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace rpfg {
namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;
bool g_fileResolved = false;

// A WIN32 subsystem app has no console, so stderr is invisible. Mirror the log
// into a file next to the executable (falling back to %TEMP%) and to the
// debugger output that Visual Studio / WinDbg shows.
//
// Note the mode is plain "a", not "a, ccs=UTF-8": ccs= switches the stream to
// wide-character I/O, and calling narrow fputs on such a stream is invalid -
// the CRT invalid-parameter handler turns that into an immediate fail-fast
// (0xC0000409). The lines below are already UTF-8, so write them as bytes.
std::FILE* logFile() {
    if (g_fileResolved) {
        return g_file;
    }
    g_fileResolved = true;

    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0) {
        std::wstring path(exePath);
        const std::size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            path.resize(slash + 1);
            path += L"rpfg.log";
            if (_wfopen_s(&g_file, path.c_str(), L"a") == 0 && g_file) {
                return g_file;
            }
        }
    }

    wchar_t tempPath[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempPath) != 0) {
        std::wstring path(tempPath);
        path += L"rpfg.log";
        _wfopen_s(&g_file, path.c_str(), L"a");
    }
    return g_file;
}

std::string timestamp() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u.%03u", local.wHour, local.wMinute,
                  local.wSecond, local.wMilliseconds);
    return buffer;
}

void write(const char* level, const std::string& message) {
    std::lock_guard<std::mutex> guard(g_mutex);
    const std::string line = "[" + timestamp() + "] [" + level + "] " + message + "\n";
    OutputDebugStringA(line.c_str());
    if (std::FILE* file = logFile()) {
        std::fputs(line.c_str(), file);
        std::fflush(file);
    }
}

}  // namespace

void logInfo(const std::string& message) {
    write("info ", message);
}

void logError(const std::string& message) {
    write("error", message);
}

}  // namespace rpfg
