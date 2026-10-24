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

// ---- 目标软件管理（托盘注入与重启的对象）----
struct Target {
    const wchar_t* key;         // config.ini [targets] 键名
    const wchar_t* exe;         // 进程名
    const wchar_t* display;     // 菜单显示名
    const wchar_t* restartArg;  // 重启时附加参数（可为 nullptr）
    const wchar_t* appPathsKey; // App Paths 注册表子键名
    const wchar_t* const* candidates;  // 候选安装路径（nullptr 结尾）
};

inline const wchar_t* const kCloudmusicCandidates[] = {
    L"D:\\LenovoSoftstore\\Install\\wangyiyunyinyue\\cloudmusic.exe",
    L"C:\\Program Files (x86)\\NetEase\\CloudMusic\\cloudmusic.exe",
    L"C:\\Program Files\\NetEase\\CloudMusic\\cloudmusic.exe",
    L"C:\\Program Files (x86)\\Netease\\CloudMusic\\cloudmusic.exe",
    L"C:\\Program Files\\Netease\\CloudMusic\\cloudmusic.exe",
    nullptr};
inline const wchar_t* const kBilibiliCandidates[] = {
    L"D:\\bilibili\\哔哩哔哩.exe",
    L"C:\\Program Files\\bilibili\\哔哩哔哩.exe",
    L"C:\\Program Files (x86)\\bilibili\\哔哩哔哩.exe",
    nullptr};

inline const Target kTargets[] = {
    {L"cloudmusic", L"cloudmusic.exe", L"网易云音乐", L"--enable-exclusive-audio",
     L"cloudmusic.exe", kCloudmusicCandidates},
    {L"bilibili", L"哔哩哔哩.exe", L"哔哩哔哩", nullptr, L"哔哩哔哩.exe", kBilibiliCandidates},
};
inline size_t TargetCount() { return sizeof(kTargets) / sizeof(kTargets[0]); }

inline int ReadTargetEnabled(size_t idx) {
    if (idx >= TargetCount()) return 0;
    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH, L"config.ini");
    return GetPrivateProfileIntW(L"targets", kTargets[idx].key, 1, path);
}

inline void SetTargetEnabled(size_t idx, int value) {
    if (idx >= TargetCount()) return;
    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH, L"config.ini");
    WritePrivateProfileStringW(L"targets", kTargets[idx].key, value ? L"1" : L"0", path);
}

inline bool IsKnownTargetExe(const wchar_t* exeName) {
    for (size_t i = 0; i < TargetCount(); ++i) {
        if (_wcsicmp(exeName, kTargets[i].exe) == 0) return true;
    }
    return false;
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
