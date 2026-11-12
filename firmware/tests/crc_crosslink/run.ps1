# 跨链路 CRC / 帧格式一致性单测（验收 3）
#
# 三份实现各自独立编译进同一个 PC 进程（不依赖 ESP-IDF）：
#   C3   firmware/c3-node/main/frame.c
#   F103 firmware/f103-node/APP/comm/{crc16,proto_frame}.c   <- 宏改名避免符号冲突
#   P4   firmware/p4-hub/components/protocol/crc16.c
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File <本文件路径>
# 退出码 0 = 全部通过，1 = 有断言失败。

$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    $d = $PSScriptRoot
    for ($i = 0; $i -lt 3; $i++) { $d = Split-Path -Parent $d }
    return $d
}

$root   = Get-RepoRoot
$c3dir  = [System.IO.Path]::Combine($root, 'firmware', 'c3-node', 'main')
$f103dir = [System.IO.Path]::Combine($root, 'firmware', 'f103-node', 'APP', 'comm')
$p4dir  = [System.IO.Path]::Combine($root, 'firmware', 'p4-hub', 'components', 'protocol')
$bld    = [System.IO.Path]::Combine($PSScriptRoot, 'build')

New-Item -ItemType Directory -Force -Path $bld | Out-Null

Write-Host ("repo : " + $root)
Write-Host ("c3   : " + $c3dir)
Write-Host ("f103 : " + $f103dir)
Write-Host ("p4   : " + $p4dir)

# mingw gcc（PATH 里没有就退回固定路径）
$gccCmd = Get-Command gcc -ErrorAction SilentlyContinue
if ($gccCmd) { $gcc = $gccCmd.Source }
else { $gcc = 'C:\Users\A\x86_64-8.1.0-release-posix-seh-rt_v6-rev0\mingw64\bin\gcc.exe' }
if (-not (Test-Path $gcc)) {
    Write-Host ("找不到 gcc: " + $gcc)
    exit 2
}
Write-Host ("gcc  : " + $gcc)

function Invoke-Gcc([string[]]$argv) {
    & $gcc @argv
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("gcc failed: " + ($argv -join ' '))
        exit $LASTEXITCODE
    }
}

$common = @('-std=gnu11', '-O2', '-Wall', '-Wextra')
$f103rename = @('-Dcrc16_compute=f103_crc16_compute', '-Dcrc16_init=f103_crc16_init')

Invoke-Gcc ($common + @('-c', "$f103dir\crc16.c", '-I', $f103dir) + $f103rename + @('-o', "$bld\f103_crc16.o"))
Invoke-Gcc ($common + @('-c', "$f103dir\proto_frame.c", '-I', $f103dir) + $f103rename + @('-o', "$bld\f103_proto_frame.o"))
Invoke-Gcc ($common + @('-c', "$p4dir\crc16.c", '-I', $p4dir, '-o', "$bld\p4_crc16.o"))
Invoke-Gcc ($common + @('-c', "$c3dir\frame.c", '-I', $c3dir, '-o', "$bld\c3_frame.o"))
Invoke-Gcc ($common + @('-c', "$PSScriptRoot\test_crc_crosslink.c", '-I', $f103dir, '-o', "$bld\test.o"))
Invoke-Gcc (@('-o', "$bld\crc_crosslink.exe",
              "$bld\test.o", "$bld\c3_frame.o",
              "$bld\f103_crc16.o", "$bld\f103_proto_frame.o", "$bld\p4_crc16.o"))

& "$bld\crc_crosslink.exe"
exit $LASTEXITCODE
