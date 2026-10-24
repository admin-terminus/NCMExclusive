@echo off
chcp 936 >nul
setlocal EnableExtensions
cd /d "%~dp0"

"%~dp0dist\NCMExTray.exe" --install
set "rc=%errorlevel%"
if not "%rc%"=="0" (
    echo [错误] 原生安装失败，错误码 %rc%。
    echo 请查看 %APPDATA%\NCMExclusive\log.txt 及安全软件提示。
    pause
    exit /b %rc%
)

echo.
echo ==========================================
echo 安装完成，托盘已启动，定向注入仅针对 cloudmusic.exe。
echo 请重新打开网易云并播放歌曲。
echo 绿色图标只表示开关状态；必须看到日志“独占模式已生效”才算成功。
echo 日志位置: %APPDATA%\NCMExclusive\log.txt
echo ==========================================
pause
exit /b 0
