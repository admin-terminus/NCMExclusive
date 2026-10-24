#!/bin/bash
# WSL 本地交叉编译脚本（免安装，工具链解包在 ~/mxroot/prefix）
# 生成全部载体变体：version / msimg32 / dwmapi / uxtheme / winmm / shlwapi /
#                  crypt32 / secur32 / wtsapi32 / psapi / iphlpapi / winhttp / dbghelp
set -e
MX=$HOME/mxroot/prefix
GCCVER=$(ls "$MX/usr/lib/gcc/x86_64-w64-mingw32" | grep -- '-win32$' | head -1)
GCCDIR="$MX/usr/lib/gcc/x86_64-w64-mingw32/$GCCVER"
MINGWDIR="$MX/usr/x86_64-w64-mingw32"
CXX="$MX/usr/bin/x86_64-w64-mingw32-g++-win32"
[ -x "$CXX" ] || CXX=$(ls "$MX"/usr/bin/x86_64-w64-mingw32-g++* | head -1)
export PATH="$MX/usr/bin:$PATH"

CARRIERS="version msimg32 dwmapi uxtheme winmm shlwapi crypt32 secur32 wtsapi32 psapi iphlpapi winhttp dbghelp"

CXXFLAGS=(-O2 -std=c++17 -Isrc -Wall
  -nostdinc -I"$GCCDIR/include/c++" -I"$GCCDIR/include/c++/backward" -I"$GCCDIR/include/c++/x86_64-w64-mingw32"
  -I"$GCCDIR/include" -I"$GCCDIR/include-fixed" -I"$MINGWDIR/include"
  -B"$GCCDIR" -B"$MINGWDIR/lib"
  -m64 -static-libgcc -static-libstdc++ -DNCMEX_CARRIER_NAME=\"generic\")
LDFLAGS=(-L"$MINGWDIR/lib" -L"$GCCDIR"
  -luser32 -lshell32 -lole32 -luuid -ladvapi32 -lpsapi)

cd "$(dirname "$0")/.."
mkdir -p dist/x64 build/defs

# 从 mingw 导入库自动生成转发 def（该导入库包含目标 DLL 的完整导出名单），
# 再补充分导入库可能缺失的隐藏导出。
NM="$MX/usr/bin/x86_64-w64-mingw32-nm"
gen_def() {
    local dll="$1" out="build/defs/$1.def"
    {
        echo "EXPORTS"
        "$NM" "$MINGWDIR/lib/lib$dll.a" 2>/dev/null | \
            sed -n 's/.* I __imp_\(.*\)$/\1/p' | \
            sed -E -e 's/^_([A-Za-z0-9_.]+)@[0-9]+$/\1/' \
                   -e 's/^@([A-Za-z0-9_.]+)@[0-9]+$/\1/' | \
            grep -E '^[A-Za-z_?@][A-Za-z0-9_.$?@]*$' | sort -u
        # 补充隐藏/新导出
        case "$dll" in
            version) echo "GetFileVersionInfoExA"; echo "GetFileVersionInfoExW"
                     echo "GetFileVersionInfoSizeExA"; echo "GetFileVersionInfoSizeExW"
                     echo "VerQueryValueExA"; echo "VerQueryValueExW" ;;
            msimg32) echo "DllInitialize"; echo "vSetDdrawident" ;;
        esac
    } | awk 'NR==1{print;next} !seen[$1]++ {print $1"='"$dll"'."$1}' > "$out"
    echo "  def $dll: $(($(grep -c '=' "$out"))) exports"
}

echo ">> CXX=$CXX ($GCCVER)"
for c in $CARRIERS; do gen_def "$c"; done

for c in $CARRIERS; do
    "$CXX" "${CXXFLAGS[@]}" -DNCMEX_CARRIER=\"$c\" -shared \
        src/hook_dll/dllmain.cpp src/hook_dll/proxy.cpp src/hook_dll/software_terminal.cpp \
        "build/defs/$c.def" -o "dist/x64/$c.dll" "${LDFLAGS[@]}"
    echo "  built dist/x64/$c.dll"
done
"$CXX" "${CXXFLAGS[@]}" -mwindows -municode src/tray/tray.cpp -o dist/NCMExTray.exe "${LDFLAGS[@]}"
# 定向注入载荷 DLL（由托盘远程 LoadLibraryW，无全局钩子导出）
"$CXX" "${CXXFLAGS[@]}" -shared src/hook_dll/dllmain.cpp src/hook_dll/proxy.cpp src/hook_dll/software_terminal.cpp \
  -o dist/ncmex_inject.dll "${LDFLAGS[@]}"

echo ">> 产物:"
ls dist/x64 | wc -l
ls -la dist/NCMExTray.exe
