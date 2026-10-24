# NCMExclusive 诊断 v3
$OutputEncoding = [System.Text.Encoding]::Default
[Console]::OutputEncoding = [System.Text.Encoding]::Default
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $env:LOCALAPPDATA 'NCMExclusive\bin'
$log = Join-Path $env:APPDATA 'NCMExclusive\log.txt'

Write-Host '===== NCMExclusive 诊断 v3 ====='
Write-Host ''
Write-Host '[1] 托盘进程'
$tray = Get-Process NCMExTray -ErrorAction SilentlyContinue
if ($tray) {
    $tray | ForEach-Object { Write-Host ("  PID={0} Path={1}" -f $_.Id, $_.Path) }
} else {
    Write-Host '  托盘未运行！' -ForegroundColor Red
}

Write-Host ''
Write-Host '[2] 实际注入 DLL 与交付文件哈希'
$latest = Get-ChildItem $bin -Filter 'ncmex_inject*.dll' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latest) {
    $installedHash = (Get-FileHash $latest.FullName -Algorithm SHA256).Hash
    Write-Host ("  安装: {0}  {1} bytes  {2}" -f $latest.FullName, $latest.Length, $latest.LastWriteTime)
    Write-Host "  安装 SHA256: $installedHash"
    $dist = Join-Path $root 'dist\ncmex_inject.dll'
    if (Test-Path $dist) {
        $distHash = (Get-FileHash $dist -Algorithm SHA256).Hash
        Write-Host "  交付 SHA256: $distHash"
        if ($distHash -eq $installedHash) {
            Write-Host '  [一致] 托盘目录中的最新 DLL 就是本次交付版本' -ForegroundColor Green
        } else {
            Write-Host '  [不一致] 请重新运行安装脚本' -ForegroundColor Red
        }
    }
} else {
    Write-Host '  未找到注入 DLL！' -ForegroundColor Red
}

Write-Host ''
Write-Host '[3] 网易云进程、命令行与注入状态'
$processes = Get-CimInstance Win32_Process -Filter "Name='cloudmusic.exe'" -ErrorAction SilentlyContinue
if ($processes) {
    foreach ($p in $processes) {
        Write-Host ("  PID={0} Parent={1}" -f $p.ProcessId, $p.ParentProcessId)
        Write-Host "  命令行: $($p.CommandLine)"
    }
} else {
    Write-Host '  cloudmusic.exe 未运行'
}
Get-Process cloudmusic* -ErrorAction SilentlyContinue | ForEach-Object {
    $loaded = '否'
    try {
        foreach ($m in $_.Modules) {
            if ($m.ModuleName -like 'ncmex*') { $loaded = $m.ModuleName; break }
        }
    } catch {}
    Write-Host ("  {0} PID={1} 注入模块={2}" -f $_.ProcessName, $_.Id, $loaded)
}

Write-Host ''
Write-Host '[4] 配置文件'
$cfg = Join-Path $env:APPDATA 'NCMExclusive\config.ini'
if (Test-Path $cfg) { Get-Content $cfg } else { Write-Host '  不存在' }

Write-Host ''
Write-Host "[5] 新日志最后 120 行: $log"
if (Test-Path $log) {
    Get-Content $log -Tail 120
} else {
    Write-Host '  不存在；托盘可能未能写入日志'
    $tempLog = Join-Path $env:TEMP 'NCMExclusive.log'
    Write-Host "  备用日志: $tempLog"
    if (Test-Path $tempLog) { Get-Content $tempLog -Tail 120 }
}

Write-Host ''
Write-Host '请完整复制以上诊断；不要只截最后几行。' -ForegroundColor Yellow
Read-Host '按回车退出'
