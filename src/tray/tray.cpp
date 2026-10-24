// NCMExclusive - 托盘控制程序：一键开/关 WASAPI 独占模式
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cerrno>
#include <string>
#include <wchar.h>

#include "../common/util.h"

namespace {

constexpr UINT kTrayMsg = WM_APP + 1;
constexpr int kIdEnable = 1001, kIdDisable = 1002, kIdRestart = 1003, kIdLog = 1004,
              kIdAutorun = 1005, kIdExit = 1006, kIdInject = 1007;
constexpr int kIdTargetBase = 2000;  // kIdTargetBase + idx 切换目标软件

HWND g_hwnd = nullptr;
HICON g_iconOn = nullptr, g_iconOff = nullptr;
UINT g_taskbarCreated = 0;
bool g_injectEnabled = true;
constexpr UINT_PTR kInjectTimer = 1;
DWORD g_injectAttempts[64] = {};
size_t g_injectAttemptNext = 0;

HICON MakeSquareIcon(COLORREF rgb) {
    constexpr int kIcon = 32;
    BYTE mask[(kIcon * kIcon) / 8] = {};
    BYTE color[kIcon * kIcon * 4] = {};
    for (size_t i = 0; i < sizeof(color); i += 4) {
        color[i] = GetBValue(rgb);
        color[i + 1] = GetGValue(rgb);
        color[i + 2] = GetRValue(rgb);
        color[i + 3] = 0xFF;
    }
    return CreateIcon(nullptr, kIcon, kIcon, 1, 32, mask, color);
}

void UpdateTray(bool enabled) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.hIcon = enabled ? g_iconOn : g_iconOff;
    nid.uCallbackMessage = kTrayMsg;
    wcscpy(nid.szTip, enabled ? L"NCMExclusive：独占开关=开（点击切换，生效以日志为准）"
                              : L"NCMExclusive：独占开关=关（点击切换）");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool IsTargetRunning(const ncmex::Target& target) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, target.exe) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return found;
}

void RestartTarget(const ncmex::Target& target) {
    wchar_t exe[MAX_PATH] = {};
    HANDLE pathSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (pathSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(pathSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, target.exe) != 0) continue;
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                             pe.th32ProcessID);
                if (process) {
                    DWORD size = MAX_PATH;
                    QueryFullProcessImageNameW(process, 0, exe, &size);
                    CloseHandle(process);
                }
                if (exe[0]) break;
            } while (Process32NextW(pathSnap, &pe));
        }
        CloseHandle(pathSnap);
    }

    const wchar_t* appPathsKeys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"};
    for (const wchar_t* prefix : appPathsKeys) {
        if (exe[0]) break;
        wchar_t key[512] = {};
        swprintf(key, 512, L"%ls%ls", prefix, target.appPathsKey);
        DWORD type = 0, size = sizeof(exe);
        RegGetValueW(HKEY_LOCAL_MACHINE, key, nullptr, RRF_RT_REG_SZ, &type, exe, &size);
    }
    if (!exe[0]) {
        for (const wchar_t* const* c = target.candidates; *c; ++c) {
            if (GetFileAttributesW(*c) != INVALID_FILE_ATTRIBUTES) {
                wcscpy(exe, *c);
                break;
            }
        }
    }
    if (!exe[0]) {
        wchar_t local[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (n) swprintf(exe, MAX_PATH, L"%ls\\bilibili\\%ls", local, target.exe);
        if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) exe[0] = 0;
    }
    if (!exe[0]) {
        wchar_t msg[512] = {};
        swprintf(msg, 512, L"未找到 %ls，请手动重启该软件。", target.exe);
        MessageBoxW(g_hwnd, msg, L"NCMExclusive", MB_ICONWARNING);
        return;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, target.exe) == 0) {
                    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID)) {
                        TerminateProcess(h, 0);
                        CloseHandle(h);
                    }
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    Sleep(1000);
    const bool enable = ncmex::ReadEnable() != 0;
    const wchar_t* arg = (enable && target.restartArg) ? target.restartArg : nullptr;
    ncmex::Log(L"[NCMExTray] 重启 %ls -> %ls (arg=%ls)", target.exe, exe,
               arg ? arg : L"(无)");
    ShellExecuteW(nullptr, L"open", exe, arg, nullptr, SW_SHOWNORMAL);
}

void ShowMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    const bool on = ncmex::ReadEnable() != 0;
    AppendMenuW(menu, MF_STRING | (on ? MF_CHECKED : 0), kIdEnable, L"开启独占模式");
    AppendMenuW(menu, MF_STRING | (!on ? MF_CHECKED : 0), kIdDisable, L"关闭独占模式");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"独占目标（单选）：");
    for (size_t i = 0; i < ncmex::TargetCount(); ++i) {
        const ncmex::Target& t = ncmex::kTargets[i];
        wchar_t label[128] = {};
        swprintf(label, 128, L"  %ls", t.display);
        const bool checked = ncmex::ReadTargetEnabled(i) != 0;
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0), kIdTargetBase + i, label);
    }
    AppendMenuW(menu, MF_STRING, kIdRestart, L"重启已选目标");
    AppendMenuW(menu, MF_STRING, kIdLog, L"打开日志目录");
    AppendMenuW(menu, MF_STRING, kIdAutorun, L"开机自启托盘");
    AppendMenuW(menu, MF_STRING | (g_injectEnabled ? MF_CHECKED : 0), kIdInject,
                L"定向注入");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdExit, L"退出");
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
}

void BalloonTip(const wchar_t* text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 3000;
    wcscpy(nid.szInfoTitle, L"NCMExclusive");
    wcscpy(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool PickInjectDll(wchar_t* out, size_t cap) {
    wchar_t dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    wchar_t pat[MAX_PATH] = {};
    swprintf(pat, MAX_PATH, L"%ls\\ncmex_inject*.dll", dir);
    WIN32_FIND_DATAW fd = {};
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return false;
    FILETIME best = {};
    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!found || CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            swprintf(out, cap, L"%ls\\%ls", dir, fd.cFileName);
            found = true;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return found;
}

bool InstalledFile(wchar_t* out, size_t cap, const wchar_t* name) {
    wchar_t local[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    return swprintf(out, cap, L"%ls\\NCMExclusive\\bin\\%ls", local, name) > 0;
}

bool EnsureInstallDirs() {
    wchar_t local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return false;
    wchar_t dir[MAX_PATH] = {};
    swprintf(dir, MAX_PATH, L"%ls\\NCMExclusive", local);
    CreateDirectoryW(dir, nullptr);
    swprintf(dir, MAX_PATH, L"%ls\\NCMExclusive\\bin", local);
    CreateDirectoryW(dir, nullptr);
    ncmex::ConfigDir();
    return true;
}

void KillForMaintenance() {
    const DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID == self) continue;
            bool hit = _wcsicmp(pe.szExeFile, L"NCMExTray.exe") == 0;
            if (!hit) hit = ncmex::IsKnownTargetExe(pe.szExeFile);
            if (!hit) continue;
            if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID)) {
                TerminateProcess(process, 0);
                CloseHandle(process);
            }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    Sleep(800);
}

bool CopyWithRetry(const wchar_t* source, const wchar_t* destination) {
    DWORD error = ERROR_SUCCESS;
    for (int i = 0; i < 20; ++i) {
        if (CopyFileW(source, destination, FALSE)) return true;
        error = GetLastError();
        Sleep(100);
    }
    ncmex::Log(L"[NCMExTray] 复制失败 %ls -> %ls err=%lu", source, destination, error);
    return false;
}

void DeleteLegacyShortcut() {
    wchar_t appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return;
    wchar_t link[MAX_PATH] = {};
    swprintf(link, MAX_PATH,
             L"%ls\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\NCMExTray.lnk", appdata);
    DeleteFileW(link);
}

bool WriteAutorun(const wchar_t* exe) {
    const LSTATUS status = RegSetKeyValueW(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"NCMExTray",
        RRF_RT_REG_SZ, exe, static_cast<DWORD>((wcslen(exe) + 1) * sizeof(wchar_t)));
    if (status == ERROR_SUCCESS) {
        DeleteLegacyShortcut();
        return true;
    }
    ncmex::Log(L"[NCMExTray] 写入开机启动失败 err=%ld", status);
    return false;
}

void DeleteAutorun() {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       L"NCMExTray");
    DeleteLegacyShortcut();
}

int InstallPackage() {
    KillForMaintenance();
    if (!EnsureInstallDirs()) return 2;

    wchar_t sourceExe[MAX_PATH] = {}, sourceDll[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, sourceExe, MAX_PATH);
    if (!PickInjectDll(sourceDll, MAX_PATH)) return 3;

    wchar_t destExe[MAX_PATH] = {}, destDll[MAX_PATH] = {}, timestampDll[MAX_PATH] = {};
    InstalledFile(destExe, MAX_PATH, L"NCMExTray.exe");
    InstalledFile(destDll, MAX_PATH, L"ncmex_inject.dll");
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t timestampName[80] = {};
    swprintf(timestampName, 80, L"ncmex_inject_%02u%02u%02u%03u.dll", now.wHour, now.wMinute,
             now.wSecond, now.wMilliseconds);
    InstalledFile(timestampDll, MAX_PATH, timestampName);

    if (!CopyWithRetry(sourceExe, destExe) || !CopyWithRetry(sourceDll, timestampDll)) return 4;
    CopyWithRetry(sourceDll, destDll);

    wchar_t config[MAX_PATH] = {};
    ncmex::ConfigPath(config, MAX_PATH, L"config.ini");
    if (GetFileAttributesW(config) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringW(L"exclusive", L"enable", L"1", config);
        WritePrivateProfileStringW(L"exclusive", L"pcm_fallback", L"1", config);
        WritePrivateProfileStringW(L"exclusive", L"balloon", L"1", config);
        WritePrivateProfileStringW(L"exclusive", L"verbose", L"0", config);
        for (size_t i = 0; i < ncmex::TargetCount(); ++i) {
            WritePrivateProfileStringW(L"targets", ncmex::kTargets[i].key, L"1", config);
        }
    }

    wchar_t log[MAX_PATH] = {}, previous[MAX_PATH] = {};
    ncmex::ConfigPath(log, MAX_PATH, L"log.txt");
    ncmex::ConfigPath(previous, MAX_PATH, L"log_previous.txt");
    MoveFileExW(log, previous, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!WriteAutorun(destExe)) return 5;

    wchar_t command[MAX_PATH + 2] = {};
    swprintf(command, MAX_PATH + 2, L"\"%ls\"", destExe);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(destExe, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        ncmex::Log(L"[NCMExTray] 安装后启动失败 err=%lu", GetLastError());
        return 6;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, 1500);
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ncmex::Log(L"[NCMExTray] 安装后托盘退出 code=%lu", code);
        return 7;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ncmex::Log(L"[NCMExTray] 原生安装完成 build=20261024-fix13-multi");
    return 0;
}

int UninstallPackage() {
    KillForMaintenance();
    DeleteAutorun();
    bool failed = false;
    wchar_t path[MAX_PATH] = {};
    InstalledFile(path, MAX_PATH, L"NCMExTray.exe");
    if (!DeleteFileW(path) && GetLastError() != ERROR_FILE_NOT_FOUND) failed = true;

    wchar_t pattern[MAX_PATH] = {};
    InstalledFile(pattern, MAX_PATH, L"ncmex_inject*.dll");
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t bin[MAX_PATH] = {}, file[MAX_PATH] = {};
            InstalledFile(bin, MAX_PATH, L"");
            swprintf(file, MAX_PATH, L"%ls%ls", bin, fd.cFileName);
            if (!DeleteFileW(file) && GetLastError() != ERROR_FILE_NOT_FOUND) failed = true;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    InstalledFile(path, MAX_PATH, L"");
    RemoveDirectoryW(path);
    ncmex::Log(L"[NCMExTray] 原生卸载完成 result=%d（配置和日志已保留）", failed ? 1 : 0);
    return failed ? 2 : 0;
}

bool MarkInjectAttempt(DWORD pid) {
    for (DWORD attempted : g_injectAttempts) {
        if (attempted == pid) return false;
    }
    g_injectAttempts[g_injectAttemptNext] = pid;
    g_injectAttemptNext = (g_injectAttemptNext + 1) % 64;
    return true;
}

void InjectProcess(DWORD pid, const wchar_t* path) {
    if (!MarkInjectAttempt(pid)) return;
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        ncmex::Log(L"[NCMExTray] 打开目标进程失败 pid=%lu err=%lu", pid, GetLastError());
        return;
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64) && wow64) {
        static bool warned = false;
        if (!warned) {
            ncmex::Log(L"[NCMExTray] 跳过 32 位目标：当前托盘为 x64");
            warned = true;
        }
        CloseHandle(process);
        return;
    }

    const SIZE_T bytes = (wcslen(path) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        ncmex::Log(L"[NCMExTray] 远程分配失败 pid=%lu err=%lu", pid, GetLastError());
        CloseHandle(process);
        return;
    }

    SIZE_T written = 0;
    HANDLE thread = nullptr;
    if (!WriteProcessMemory(process, remote, path, bytes, &written) || written != bytes) {
        ncmex::Log(L"[NCMExTray] 写入 DLL 路径失败 pid=%lu err=%lu", pid, GetLastError());
    } else {
        FARPROC loadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        thread = CreateRemoteThread(process, nullptr, 0,
                                    reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), remote,
                                    0, nullptr);
        if (!thread) ncmex::Log(L"[NCMExTray] 创建远程加载线程失败 pid=%lu err=%lu", pid,
                                GetLastError());
    }

    if (thread) {
        const DWORD wait = WaitForSingleObject(thread, 10000);
        if (wait != WAIT_OBJECT_0) {
            ncmex::Log(L"[NCMExTray] 远程加载等待失败 pid=%lu wait=0x%lx err=%lu", pid, wait,
                       GetLastError());
            CloseHandle(thread);
            CloseHandle(process);
            return;
        }
        DWORD module = 0;
        GetExitCodeThread(thread, &module);
        CloseHandle(thread);
        if (module) {
            ncmex::Log(L"[NCMExTray] 定向注入成功 pid=%lu", pid);
        } else {
            ncmex::Log(L"[NCMExTray] LoadLibraryW 在目标进程内失败 pid=%lu", pid);
        }
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
}

bool InjectRunningTargets() {
    if (!g_injectEnabled) return true;
    wchar_t path[MAX_PATH] = {};
    if (!PickInjectDll(path, MAX_PATH)) return false;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snapshot, &pe)) {
        do {
            for (size_t i = 0; i < ncmex::TargetCount(); ++i) {
                if (!ncmex::ReadTargetEnabled(i)) continue;
                if (_wcsicmp(pe.szExeFile, ncmex::kTargets[i].exe) == 0) {
                    InjectProcess(pe.th32ProcessID, path);
                    break;
                }
            }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return true;
}

bool StartInjection() {
    wchar_t path[MAX_PATH] = {};
    if (!PickInjectDll(path, MAX_PATH)) {
        ncmex::Log(L"[NCMExTray] 未找到 ncmex_inject*.dll");
        return false;
    }
    g_injectEnabled = true;
    memset(g_injectAttempts, 0, sizeof(g_injectAttempts));
    g_injectAttemptNext = 0;
    wchar_t names[256] = {};
    for (size_t i = 0; i < ncmex::TargetCount(); ++i) {
        if (ncmex::ReadTargetEnabled(i)) {
            if (names[0]) wcscat(names, L", ");
            wcscat(names, ncmex::kTargets[i].exe);
        }
    }
    ncmex::Log(L"[NCMExTray] 定向注入已开启（目标: %ls），DLL: %ls", names, path);
    return InjectRunningTargets();
}

void StopInjection() {
    const bool wasEnabled = g_injectEnabled;
    g_injectEnabled = false;
    if (wasEnabled) ncmex::Log(L"[NCMExTray] 定向注入已停用（已注入的目标软件需重启卸载）");
}

void ToggleInjection() {
    if (g_injectEnabled) {
        StopInjection();
        BalloonTip(L"已停用定向注入（已注入的软件需重启卸载）");
    } else {
        if (StartInjection()) BalloonTip(L"定向注入已开启");
        else BalloonTip(L"注入开启失败，请查看日志（可能被安全软件拦截）");
    }
}

void RestartEnabledTargets(bool onlyRunning) {
    for (size_t i = 0; i < ncmex::TargetCount(); ++i) {
        if (!ncmex::ReadTargetEnabled(i)) continue;
        if (onlyRunning && !IsTargetRunning(ncmex::kTargets[i])) continue;
        RestartTarget(ncmex::kTargets[i]);
    }
}

void ToggleExclusive() {
    const bool newState = ncmex::ReadEnable() == 0;
    ncmex::SetEnable(newState ? 1 : 0);
    UpdateTray(newState);
    ncmex::Log(L"[NCMExTray] 用户切换：enable=%d", newState ? 1 : 0);
    bool anyRunning = false;
    for (size_t i = 0; i < ncmex::TargetCount(); ++i) {
        if (ncmex::ReadTargetEnabled(i) && IsTargetRunning(ncmex::kTargets[i])) {
            anyRunning = true;
            break;
        }
    }
    if (anyRunning) {
        BalloonTip(newState ? L"已开启独占模式，正在重启所选软件"
                            : L"已关闭独占模式，正在恢复共享播放");
        RestartEnabledTargets(true);
    } else {
        BalloonTip(newState ? L"已开启独占模式（打开所选软件后生效）"
                            : L"已关闭独占模式");
    }
}

void ToggleTarget(size_t idx) {
    const ncmex::Target& t = ncmex::kTargets[idx];
    if (ncmex::ReadTargetEnabled(idx)) {
        // 已选中：单击不取消（单选至少要保留一项）
        BalloonTip(L"至少保留一个独占目标；如需更换请直接点击另一个");
        return;
    }
    // 单选互斥：先停用其它目标并重启以卸载注入
    for (size_t j = 0; j < ncmex::TargetCount(); ++j) {
        if (j == idx || !ncmex::ReadTargetEnabled(j)) continue;
        ncmex::SetTargetEnabled(j, 0);
        ncmex::Log(L"[NCMExTray] 目标 %ls -> 0（互斥切换）", ncmex::kTargets[j].exe);
        if (IsTargetRunning(ncmex::kTargets[j])) RestartTarget(ncmex::kTargets[j]);
    }
    ncmex::SetTargetEnabled(idx, 1);
    ncmex::Log(L"[NCMExTray] 目标 %ls -> 1", t.exe);
    wchar_t text[256] = {};
    swprintf(text, 256, L"独占目标已切换为 %ls", t.display);
    BalloonTip(text);
    if (IsTargetRunning(t)) RestartTarget(t);  // 重启以套用注入
}

void SetAutorun(bool enable) {
    if (enable) {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        WriteAutorun(self);
    } else {
        DeleteAutorun();
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_iconOn = MakeSquareIcon(RGB(0x30, 0xC0, 0x30));
            g_iconOff = MakeSquareIcon(RGB(0x90, 0x90, 0x90));
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
            nid.hIcon = ncmex::ReadEnable() ? g_iconOn : g_iconOff;
            nid.uCallbackMessage = kTrayMsg;
            wcscpy(nid.szTip, L"NCMExclusive（点击切换独占模式）");
            Shell_NotifyIconW(NIM_ADD, &nid);
            StartInjection();
            SetTimer(hwnd, kInjectTimer, 1000, nullptr);
            break;
        }
        case WM_TIMER:
            if (wp == kInjectTimer) InjectRunningTargets();
            break;
        case kTrayMsg:
            if (lp == WM_LBUTTONUP) ToggleExclusive();
            else if (lp == WM_RBUTTONUP) ShowMenu();
            break;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdEnable: if (!ncmex::ReadEnable()) ToggleExclusive(); break;
                case kIdDisable: if (ncmex::ReadEnable()) ToggleExclusive(); break;
                case kIdRestart: RestartEnabledTargets(false); break;
                case kIdLog:
                    ShellExecuteW(nullptr, L"open", ncmex::ConfigDir(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                    break;
                case kIdAutorun: SetAutorun(true); break;
                case kIdInject: ToggleInjection(); break;
                case kIdExit: DestroyWindow(hwnd); break;
                default:
                    if (LOWORD(wp) >= kIdTargetBase &&
                        LOWORD(wp) < kIdTargetBase + ncmex::TargetCount()) {
                        ToggleTarget(LOWORD(wp) - kIdTargetBase);
                    }
                    break;
            }
            break;
        default:
            if (g_taskbarCreated && msg == g_taskbarCreated) {
                // 资源管理器重启后重建托盘图标
                NOTIFYICONDATAW nid{};
                nid.cbSize = sizeof(nid);
                nid.hWnd = hwnd;
                nid.uID = 1;
                nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
                nid.hIcon = ncmex::ReadEnable() ? g_iconOn : g_iconOff;
                nid.uCallbackMessage = kTrayMsg;
                wcscpy(nid.szTip, L"NCMExclusive（点击切换独占模式）");
                Shell_NotifyIconW(NIM_ADD, &nid);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR commandLine, int) {
    if (commandLine && wcscmp(commandLine, L"--install") == 0) return InstallPackage();
    if (commandLine && wcscmp(commandLine, L"--uninstall") == 0) return UninstallPackage();

    // 调试自检：验证日志路径可写，失败则弹窗显示路径与错误码
    {
        wchar_t probe[MAX_PATH] = {};
        ncmex::ConfigPath(probe, MAX_PATH, L"log.txt");
        if (FILE* fp = _wfopen(probe, L"a+b")) {
            fclose(fp);
        } else {
            const int errMain = errno;
            wchar_t tmp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tmp);
            wchar_t alt[MAX_PATH] = {};
            swprintf(alt, MAX_PATH, L"%lsNCMExclusive.log", tmp);
            FILE* fp2 = _wfopen(alt, L"a+b");
            const int errAlt = errno;
            wchar_t msg[2048] = {};
            swprintf(msg, 2048,
                     L"[NCMEx 自检] 日志写入失败\n\n主路径: %ls\n错误: %d (%ls)\n\n备用: %ls\n结果: %ls (err=%d)\n\n请截图给开发者",
                     probe, errMain, _wcserror(errMain), alt, fp2 ? L"OK" : L"FAIL", errAlt);
            MessageBoxW(nullptr, msg, L"NCMExclusive 调试", MB_ICONWARNING);
            if (fp2) fclose(fp2);
        }
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"NCMExTray_SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    ncmex::Log(L"[NCMExTray] 托盘已启动 build=20261024-fix13-multi enable=%d",
               ncmex::ReadEnable());

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"NCMExTray";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"NCMExTray", 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    StopInjection();
    if (g_iconOn) DestroyIcon(g_iconOn);
    if (g_iconOff) DestroyIcon(g_iconOff);
    ReleaseMutex(mutex);
    return 0;
}
