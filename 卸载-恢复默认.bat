@echo off
chcp 936 >nul
setlocal EnableExtensions
cd /d "%~dp0"

"%~dp0dist\NCMExTray.exe" --uninstall
set "rc=%errorlevel%"
if not "%rc%"=="0" (
    echo [错误] 卸载未完全成功，错误码 %rc%。
    echo 请重启电脑后再运行一次；配置和日志会保留。
    pause
    exit /b %rc%
)

echo.
echo 卸载完成。配置和日志保留在 %APPDATA%\NCMExclusive。
echo 请重新启动网易云音乐；若旧全局注入版曾注入其他程序，建议重启电脑。
pause
exit /b 0
