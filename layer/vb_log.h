// vb_log.h -- the layer's log: one timestamped line appended to %LOCALAPPDATA%\VRBench\layer.log per call.
// The file is opened and closed for every line, so a game that crashes loses nothing already written.
// Nothing here is per frame: callers log state changes only (a file re-read, a haptic pulse, the first sync).
#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace vb {

// %LOCALAPPDATA%\VRBench\<name>; false when LOCALAPPDATA is not set (then nothing is logged or read)
inline bool state_path(const wchar_t* name, wchar_t* out, DWORD room) {
    wchar_t base[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) return false;
    return _snwprintf_s(out, room, _TRUNCATE, L"%s\\VRBench\\%s", base, name) > 0;
}

inline void vb_log(const char* fmt, ...) {
    wchar_t path[MAX_PATH]{};
    if (!state_path(L"layer.log", path, MAX_PATH)) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || f == nullptr) return;   // the folder is the driver's to make
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::fprintf(f, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}

} // namespace vb
