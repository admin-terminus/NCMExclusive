// NCMExclusive - 公共小工具：配置目录、ini 读写、日志
#pragma once

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace ncmex {

// 返回 "%APPDATA%\NCMExclusive" 目录（末尾无反斜杠），并确保目录存在
inline const wchar_t* ConfigDir() {
    static wchar_t dir[MAX_PATH] = {};
    if (dir[0]) return dir;
    wchar_t appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) || !appdata[0]) {
        GetTempPathW(MAX_PATH, appdata);
    }
    swprintf(dir, MAX_PATH, L"%ls\\NCMExclusive", appdata);
    CreateDirectoryW(dir, nullptr);
    return dir;
}

inline void ConfigPath(wchar_t* out, size_t cap, const wchar_t* file) {
    swprintf(out, cap, L"%ls\\%ls", ConfigDir(), file);
}

struct Config {
    int enable = 1;         // 总开关：1=独占 0=原样共享
    int pcm_fallback = 1;   // float32 独占不支持时，改 PCM32 独占 + 渲染端 float->int32 转换
    int balloon = 1;        // 首次独占生效时托盘气泡提示
    int verbose = 0;        // 详细日志
};

inline Config LoadConfig() {
    Config c;
    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH, L"config.ini");
    c.enable = GetPrivateProfileIntW(L"exclusive", L"enable", 1, path);
    c.pcm_fallback = GetPrivateProfileIntW(L"exclusive", L"pcm_fallback", 1, path);
    c.balloon = GetPrivateProfileIntW(L"exclusive", L"balloon", 1, path);
    c.verbose = GetPrivateProfileIntW(L"exclusive", L"verbose", 0, path);
    return c;
}

inline void SetEnable(int value) {
    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH, L"config.ini");
    WritePrivateProfileStringW(L"exclusive", L"enable", value ? L"1" : L"0", path);
}

inline int ReadEnable() {
    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH, L"config.ini");
    return GetPrivateProfileIntW(L"exclusive", L"enable", 1, path);
}

inline void Log(const wchar_t* fmt, ...) {
    wchar_t line[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    const int prefix = _snwprintf(line, 2046,
                                  L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", st.wYear, st.wMonth,
                                  st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (prefix < 0) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(line + prefix, 2046 - prefix, fmt, ap);
    va_end(ap);

    size_t length = 0;
    while (length < 2046 && line[length]) ++length;
    line[length++] = L'\r';
    line[length++] = L'\n';

    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH, L"log.txt");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                          FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        swprintf(path, MAX_PATH, L"%lsNCMExclusive.log", tmp);
        file = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    BYTE bytes[4098];
    DWORD count = 0;
    if (size.QuadPart == 0) {
        bytes[count++] = 0xFF;
        bytes[count++] = 0xFE;
    }
    CopyMemory(bytes + count, line, static_cast<DWORD>(length * sizeof(wchar_t)));
    count += static_cast<DWORD>(length * sizeof(wchar_t));
    DWORD written = 0;
    WriteFile(file, bytes, count, &written, nullptr);
    CloseHandle(file);
}

}  // namespace ncmex
