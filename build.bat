@echo off
chcp 936 >nul
setlocal EnableExtensions EnableDelayedExpansion
title NCMExclusive 一键编译打包

cd /d "%~dp0"
if not exist dist mkdir dist
if not exist dist\x64 mkdir dist\x64
if not exist build\msvc mkdir build\msvc

rem 优先使用 VS2022 / Build Tools 的 x64 MSVC
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
)
if defined VSINSTALL (
    echo 使用 MSVC: !VSINSTALL!
    call "!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat" >nul
    set "CXXFLAGS=/nologo /O2 /MD /std:c++17 /EHsc /Isrc /W3 /Fo:build\msvc\"
    set "LIBS=user32.lib shell32.lib ole32.lib uuid.lib advapi32.lib psapi.lib"

    cl !CXXFLAGS! /LD src\hook_dll\dllmain.cpp src\hook_dll\proxy.cpp src\hook_dll\software_terminal.cpp /Fe:dist\x64\version.dll /link /DEF:src\hook_dll\exports_version.def !LIBS! || goto :fail
    cl !CXXFLAGS! /LD src\hook_dll\dllmain.cpp src\hook_dll\proxy.cpp src\hook_dll\software_terminal.cpp /Fe:dist\x64\msimg32.dll /link /DEF:src\hook_dll\exports_msimg32.def !LIBS! || goto :fail
    cl !CXXFLAGS! /LD src\hook_dll\dllmain.cpp src\hook_dll\proxy.cpp src\hook_dll\software_terminal.cpp /Fe:dist\ncmex_inject.dll /link !LIBS! || goto :fail
    cl !CXXFLAGS! src\tray\tray.cpp /Fe:dist\NCMExTray.exe /link /SUBSYSTEM:WINDOWS !LIBS! || goto :fail
    goto :package
)

rem 没有 MSVC 时退化到 PATH 中的 mingw-w64 g++
where g++ >nul 2>nul
if not errorlevel 1 (
    echo 使用 mingw-w64 g++
    set "CXXFLAGS=-O2 -std=c++17 -Isrc -Wall -m64 -static -static-libgcc -static-libstdc++"
    set "LIBS=-luser32 -lshell32 -lole32 -luuid -ladvapi32 -lpsapi"
    g++ !CXXFLAGS! -shared src\hook_dll\dllmain.cpp src\hook_dll\proxy.cpp src\hook_dll\software_terminal.cpp src\hook_dll\exports_version.def -o dist\x64\version.dll !LIBS! || goto :fail
    g++ !CXXFLAGS! -shared src\hook_dll\dllmain.cpp src\hook_dll\proxy.cpp src\hook_dll\software_terminal.cpp src\hook_dll\exports_msimg32.def -o dist\x64\msimg32.dll !LIBS! || goto :fail
    g++ !CXXFLAGS! -shared src\hook_dll\dllmain.cpp src\hook_dll\proxy.cpp src\hook_dll\software_terminal.cpp -o dist\ncmex_inject.dll !LIBS! || goto :fail
    g++ !CXXFLAGS! -mwindows -municode src\tray\tray.cpp -o dist\NCMExTray.exe !LIBS! || goto :fail
    goto :package
)

echo [错误] 未找到编译器。请安装以下任一：
echo   - Visual Studio 2022 或 Build Tools for Visual Studio 2022（含 C++ 桌面开发）
echo   - MinGW-w64 g++
echo 也可以直接使用 dist\ 目录里已编译好的产物。
goto :fail

:package
echo.
echo 正在打包 NCMExclusive-win-x64.zip ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$r=(Get-Location).Path; $items=@('dist','src','安装-开启独占模式.bat','卸载-恢复默认.bat','诊断-收集信息.bat','诊断v2-注入检测.bat','诊断v2-注入检测.ps1','check_imports.ps1','README.md','LICENSE','THIRD_PARTY_NOTICES.md','build.bat'); Compress-Archive -Path ($items | ForEach-Object { Join-Path $r $_ }) -DestinationPath (Join-Path $r 'NCMExclusive-win-x64.zip') -Force" || goto :fail

echo.
echo ==========================================
echo 编译打包完成！
echo 主程序: dist\NCMExTray.exe
echo 注入组件: dist\ncmex_inject.dll
echo 完整包: %CD%\NCMExclusive-win-x64.zip
echo 注意: Windows 脚本构建 x64 主组件及 version/msimg32 备用载体；
echo       完整 13 种载体与 x86 版本请用 WSL 的 src/build_wsl*.sh 构建。
echo ==========================================
pause
exit /b 0

:fail
echo.
echo [错误] 编译或打包失败，请把上方错误信息反馈给开发者。
pause
exit /b 1
