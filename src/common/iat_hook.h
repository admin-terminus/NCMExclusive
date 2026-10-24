// NCMExclusive - 极简 PE 导入表（IAT）钩子：替换指定模块对某个 DLL 函数的导入
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>

#ifndef NCMEX_NO_PSAPI
#include <psapi.h>
#endif

namespace ncmex {

struct ImageImportDescriptor_ {
    uint32_t OriginalFirstThunk;  // RVA -> INT
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;       // RVA -> DLL 名
    uint32_t FirstThunk; // RVA -> IAT
};

// 把 importDll 传 nullptr 即可匹配所有 DLL 的导入（按函数名）。
// 全量 RVA 边界检查：本函数可能被 worker 线程在进程加载 DLL 的窗口期调用，
// 任何越界读取都会引发 AV，因此所有指针解引用前必须验证落在 SizeOfImage 内。
inline bool IatHook(HMODULE module, const char* importDll, const char* fnName, void* detour,
                    void** originalOut) {
    if (!module || !fnName || !detour) return false;

    auto* dos = reinterpret_cast<uint8_t*>(module);
    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(dos);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dosHeader->e_lfanew <= 0 || dosHeader->e_lfanew > 0x100000) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(dos + dosHeader->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
    const auto valid = [imageSize](uint32_t rva, size_t need) {
        return rva > 0 && static_cast<uint64_t>(rva) + need <= imageSize;
    };

    const uint32_t importRva =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!valid(importRva, sizeof(ImageImportDescriptor_))) return false;

    auto rvaToPtr = [dos](uint32_t rva) -> uint8_t* { return dos + rva; };
    auto* desc = reinterpret_cast<ImageImportDescriptor_*>(rvaToPtr(importRva));

    for (int guardDesc = 0; desc->Name && guardDesc < 512; ++desc, ++guardDesc) {
        if (!valid(desc->Name, 1)) break;
        const char* dllName = reinterpret_cast<const char*>(rvaToPtr(desc->Name));
        if (importDll && _stricmp(dllName, importDll) != 0) continue;

        const uint32_t thunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                           : desc->FirstThunk;
        if (!valid(thunkRva, sizeof(uintptr_t))) continue;
        auto* thunk = reinterpret_cast<uintptr_t*>(rvaToPtr(thunkRva));
        auto* iat = reinterpret_cast<uintptr_t*>(rvaToPtr(desc->FirstThunk));
        for (int i = 0; i < 4096; ++i) {
            if (!valid(thunkRva + static_cast<uint32_t>(i) * sizeof(uintptr_t),
                       sizeof(uintptr_t)))
                break;
            const uintptr_t ordinalFlag = sizeof(uintptr_t) == 8 ? IMAGE_ORDINAL_FLAG64
                                                                 : IMAGE_ORDINAL_FLAG32;
            if (thunk[i] & ordinalFlag) continue;
            const uint32_t nameRva = static_cast<uint32_t>(thunk[i] & 0x7FFFFFFF);
            if (!valid(nameRva, sizeof(IMAGE_IMPORT_BY_NAME))) break;
            auto* nameEntry = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(rvaToPtr(nameRva));
            if (std::strcmp(nameEntry->Name, fnName) != 0) continue;

            if (!valid(desc->FirstThunk + static_cast<uint32_t>(i) * sizeof(uintptr_t),
                       sizeof(uintptr_t)))
                break;
            void* original = reinterpret_cast<void*>(iat[i]);
            if (original == detour) return true;

            DWORD oldProtect = 0;
            if (!VirtualProtect(&iat[i], sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect))
                return false;
            if (originalOut && (!*originalOut || *originalOut == detour)) *originalOut = original;
            InterlockedExchangePointer(reinterpret_cast<volatile PVOID*>(&iat[i]), detour);
            DWORD ignored = 0;
            VirtualProtect(&iat[i], sizeof(uintptr_t), oldProtect, &ignored);
            return true;
        }
    }
    return false;
}

}  // namespace ncmex
