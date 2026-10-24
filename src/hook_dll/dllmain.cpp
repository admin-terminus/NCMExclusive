// NCMExclusive - 注入载荷 DLL 入口
// 由托盘通过 CreateRemoteThread + LoadLibraryW 定向注入 cloudmusic.exe；
// 其它进程不加载本 DLL，也不启动 worker。
#include <windows.h>

#include "proxy.h"
#include "../common/util.h"

namespace {

DWORD WINAPI WorkerThread(void*) {
    ncmex::Log(L"[NCMEx] worker 线程已启动，开始安装钩子");
    // 持续轮询安装 45 秒，覆盖迟加载模块；IatHook 对重复安装保持幂等。
    for (int round = 0; round < 45; ++round) {
        ncmex::InstallHooks();
        Sleep(1000);
    }
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(instance);
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const wchar_t* name = wcsrchr(exe, L'\\');
            name = name ? name + 1 : exe;
            if (_wcsicmp(name, L"cloudmusic.exe") != 0) return TRUE;

            ncmex::Log(L"[NCMEx] 已注入网易云 build=20261024-fix14-rollback pid=%lu exe=%ls",
                       GetCurrentProcessId(), exe);
            if (HANDLE t = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr)) {
                CloseHandle(t);
                ncmex::Log(L"[NCMEx] worker 线程已创建");
            } else {
                ncmex::Log(L"[NCMEx] CreateThread 失败 err=%lu", GetLastError());
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
