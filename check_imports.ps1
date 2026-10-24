# check_imports.ps1 - Inspect a PE file's import table for version.dll / msimg32.dll
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File check_imports.ps1 -Exe <path>
# Output: "version" | "msimg32" | "none"
# With -Full: first line is arch ("x64"|"x86"|"?"), second line is the variant above.
param([string]$Exe, [switch]$Full)

$bytes = [System.IO.File]::ReadAllBytes($Exe)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$coff = $pe + 4
$numSections = [BitConverter]::ToUInt16($bytes, $coff + 2)
$optSize = [BitConverter]::ToUInt16($bytes, $coff + 16)
$opt = $coff + 20
$machine = [BitConverter]::ToUInt16($bytes, $coff)
$magic = [BitConverter]::ToUInt16($bytes, $opt)
if ($Full) {
    $arch = switch ($machine) { 0x8664 { 'x64' } 0x14c { 'x86' } default { '?' } }
    Write-Output $arch
}
$importRva = 0
if ($magic -eq 0x20B) {
    $importRva = [BitConverter]::ToUInt32($bytes, $opt + 112)  # PE32+ data dir[1]
} elseif ($magic -eq 0x10B) {
    $importRva = [BitConverter]::ToUInt32($bytes, $opt + 104)  # PE32 data dir[1]
}
if ($importRva -eq 0) { Write-Output 'none'; exit 0 }

$secStart = $opt + $optSize
function RvaToOff([uint32]$rva) {
    for ($i = 0; $i -lt $script:numSections; $i++) {
        $s = $script:secStart + 40 * $i
        $va = [BitConverter]::ToUInt32($script:bytes, $s + 12)
        $rawSize = [BitConverter]::ToUInt32($script:bytes, $s + 16)
        $rawOff = [BitConverter]::ToUInt32($script:bytes, $s + 20)
        if ($rva -ge $va -and $rva -lt ($va + [Math]::Max($rawSize, 1))) {
            return $rawOff + ($rva - $va)
        }
    }
    return 0
}

$names = @()
$off = RvaToOff $importRva
while ($off -ne 0 -and ($off + 20) -le $bytes.Length) {
    $nameRva = [BitConverter]::ToUInt32($bytes, $off + 12)
    if ($nameRva -eq 0) { break }
    $nameOff = RvaToOff $nameRva
    if ($nameOff -eq 0) { break }
    $end = [Array]::IndexOf($bytes, [byte]0, $nameOff)
    if ($end -lt 0) { break }
    $names += [System.Text.Encoding]::ASCII.GetString($bytes, $nameOff, $end - $nameOff).ToLower()
    $off += 20
}
if ($names -contains 'version.dll') { Write-Output 'version' }
elseif ($names -contains 'msimg32.dll') { Write-Output 'msimg32' }
else { Write-Output 'none' }
