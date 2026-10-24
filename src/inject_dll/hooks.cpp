// 旧全局钩子实现的源码存档，当前所有构建脚本均不编译此文件。
// 当前方案只在 cloudmusic.exe 内定向 LoadLibraryW，不使用这些导出。
#include <windows.h>

extern "C" {

__declspec(dllexport) LRESULT CALLBACK NcmExGetMsgProc(int code, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

__declspec(dllexport) LRESULT CALLBACK NcmExCbtProc(int code, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // extern "C"
