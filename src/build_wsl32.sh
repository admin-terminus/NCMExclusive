#!/bin/bash
# WSL 交叉编译 32 位 (i686) 全部载体变体
set -e
MX=$HOME/mxroot32/prefix
GCCVER=$(ls "$MX/usr/lib/gcc/i686-w64-mingw32" | grep -- '-win32$' | head -1)
GCCDIR="$MX/usr/lib/gcc/i686-w64-mingw32/$GCCVER"
MINGWDIR="$MX/usr/i686-w64-mingw32"
CXX="$MX/usr/bin/i686-w64-mingw32-g++-win32"
[ -x "$CXX" ] || CXX=$(ls "$MX"/usr/bin/i686-w64-mingw32-g++* | head -1)
export PATH="$MX/usr/bin:$PATH"

CARRIERS="version msimg32 dwmapi uxtheme winmm shlwapi crypt32 secur32 wtsapi32 psapi iphlpapi winhttp dbghelp"

CXXFLAGS=(-O2 -std=c++17 -Isrc -Wall
  -nostdinc -I"$GCCDIR/include/c++" -I"$GCCDIR/include/c++/backward" -I"$GCCDIR/include/c++/i686-w64-mingw32"
  -I"$GCCDIR/include" -I"$GCCDIR/include-fixed" -I"$MINGWDIR/include"
  -B"$GCCDIR" -B"$MINGWDIR/lib"
  -static-libgcc -static-libstdc++ -DNCMEX_CARRIER_NAME=\"generic\")
LDFLAGS=(-L"$MINGWDIR/lib" -L"$GCCDIR"
  -luser32 -lshell32 -lole32 -luuid -ladvapi32 -lpsapi)

cd "$(dirname "$0")/.."
mkdir -p dist/x86 build/defs32

NM="$MX/usr/bin/i686-w64-mingw32-nm"
gen_def() {
    local dll="$1" out="build/defs32/$1.def"
    {
        echo "EXPORTS"
        "$NM" "$MINGWDIR/lib/lib$dll.a" 2>/dev/null | \
            sed -n 's/.* I __imp_\(.*\)$/\1/p' | \
            sed -E -e 's/^_([A-Za-z0-9_.]+)@[0-9]+$/\1/' \
                   -e 's/^@([A-Za-z0-9_.]+)@[0-9]+$/\1/' | \
            grep -E '^[A-Za-z_?@][A-Za-z0-9_.$?@]*$' | sort -u
        case "$dll" in
            version) echo "GetFileVersionInfoExA"; echo "GetFileVersionInfoExW"
                     echo "GetFileVersionInfoSizeExA"; echo "GetFileVersionInfoSizeExW"
                     echo "VerQueryValueExA"; echo "VerQueryValueExW" ;;
            msimg32) echo "DllInitialize"; echo "vSetDdrawident" ;;
        esac
    } | awk 'NR==1{print;next} !seen[$1]++ {print $1"='"$dll"'."$1}' > "$out"
    echo "  def $dll: $(grep -c '=' "$out") exports"
}

echo ">> CXX=$CXX ($GCCVER)"
for c in $CARRIERS; do gen_def "$c"; done
for c in $CARRIERS; do
    "$CXX" "${CXXFLAGS[@]}" -DNCMEX_CARRIER=\"$c\" -shared \
        src/hook_dll/dllmain.cpp src/hook_dll/proxy.cpp src/hook_dll/software_terminal.cpp \
        "build/defs32/$c.def" -o "dist/x86/$c.dll" "${LDFLAGS[@]}"
    echo "  built dist/x86/$c.dll"
done
# 32 位定向注入载荷（需配套 32 位托盘）
"$CXX" "${CXXFLAGS[@]}" -shared src/hook_dll/dllmain.cpp src/hook_dll/proxy.cpp src/hook_dll/software_terminal.cpp \
  -o dist/x86/ncmex_inject.dll "${LDFLAGS[@]}"
ls dist/x86 | wc -l
