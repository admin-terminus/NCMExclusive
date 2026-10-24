@echo off
chcp 936 >nul
cd /d "%~dp0"
setlocal EnableExtensions EnableDelayedExpansion
title NCMExclusive 诊断

echo ====== NCMExclusive 诊断信息收集 ======
echo.

rem ---- 1. 定位网易云 ----
set "NCMDIR="
for %%K in (
    "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\cloudmusic.exe"
    "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\cloudmusic.exe"
) do (
    if not defined NCMDIR (
        for /f "tokens=1,2*" %%A in ('reg query %%K /ve 2^>nul ^| findstr /C:"REG_SZ"') do (
            if not defined NCMDIR set "NCMTMP=%%C"
        )
    )
)
if defined NCMTMP (
    for %%I in ("%NCMTMP%") do set "NCMDIR=%%~dpI"
)
if not defined NCMDIR (
    for %%D in (
        "%ProgramFiles(x86)%\NetEase\CloudMusic"
        "%ProgramFiles%\NetEase\CloudMusic"
        "%LOCALAPPDATA%\NetEase\CloudMusic"
        "%LOCALAPPDATA%\Netease\CloudMusic"
    ) do (
        if not defined NCMDIR if exist "%%~D\cloudmusic.exe" set "NCMDIR=%%~D"
    )
)
echo [1] 网易云目录: %NCMDIR%
if not defined NCMDIR ( echo     未找到 cloudmusic.exe！请手动编辑本脚本填写 NCMDIR。 & goto :sec2 )
echo     cloudmusic.exe 信息:
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check_imports.ps1" -Exe "%NCMDIR%\cloudmusic.exe" -Full | findstr /N "^"

:sec2
echo.
echo [2] 代理 DLL 状态:
if defined NCMDIR (
    for %%V in (version.dll msimg32.dll) do (
        if exist "%NCMDIR%\%%V" ( echo     [已安装] %NCMDIR%\%%V
            powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check_imports.ps1" -Exe "%NCMDIR%\%%V" -Full | findstr /N "^"
        ) else ( echo     [缺失]   %NCMDIR%\%%V )
    )
) else ( echo     跳过 )

echo.
echo [3] 托盘进程:
tasklist 2>nul | findstr /I "NCMExTray" || echo     NCMExTray.exe 未在运行

echo.
echo [4] 配置文件 (%APPDATA%\NCMExclusive\config.ini):
if exist "%APPDATA%\NCMExclusive\config.ini" ( type "%APPDATA%\NCMExclusive\config.ini" ) else ( echo     配置文件不存在！ )

echo.
echo [5] 日志 (%APPDATA%\NCMExclusive\log.txt) —— 最关键：
if exist "%APPDATA%\NCMExclusive\log.txt" ( type "%APPDATA%\NCMExclusive\log.txt" ) else ( echo     日志不存在，说明代理 DLL 从未被加载！ )

echo.
echo [6] 网易云进程:
tasklist 2>nul | findstr /I "cloudmusic" || echo     cloudmusic.exe 未在运行

echo.
echo ============================================
echo 请把以上全部内容截图或复制给开发者。
echo ============================================
pause
