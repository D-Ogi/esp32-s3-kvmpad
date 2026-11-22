# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

param([switch]$BuildOnly, [switch]$Inspect, [string]$Serial = $env:KVMPAD_USB_SERIAL)
$ErrorActionPreference = 'Stop'
if (!$BuildOnly -and $Serial -notmatch '^[0-9A-Fa-f]{12}$') {
    throw 'Pass the 12-digit board USB serial using -Serial or KVMPAD_USB_SERIAL.'
}
$workspace = (Get-Item (Join-Path $PSScriptRoot '../../..')).FullName
$artifact = Join-Path $workspace 'evidence/build'
$evidence = Join-Path $workspace 'evidence/hardware'
$exe = Join-Path $artifact 'hpe-get-windows.exe'
$source = Join-Path $PSScriptRoot 'hpe_get_windows.cpp'
New-Item -ItemType Directory -Force $artifact, $evidence | Out-Null
if ($BuildOnly -or !(Test-Path -LiteralPath $exe) -or
    (Get-Item -LiteralPath $source).LastWriteTimeUtc -gt (Get-Item -LiteralPath $exe).LastWriteTimeUtc) {
    $cc = Join-Path $workspace '.tools/llvm-mingw-20261020-ucrt-x86_64/bin/clang++.exe'
    & $cc -O2 -Wall -Wextra -Werror -static $source -lsetupapi -lhid -lcfgmgr32 -o $exe
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    if ($BuildOnly) { Write-Output "Compiled $exe"; exit 0 }
}
$log = Join-Path $evidence ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-hpe-get-windows.log')
$options = @{ FilePath = $exe; WindowStyle = 'Hidden'; PassThru = $true; RedirectStandardOutput = $log; ArgumentList = @($Serial) }
if ($Inspect) { $options.ArgumentList += '--inspect' }
$probe = Start-Process @options
$finished = $probe.WaitForExit(12000)
if (!$finished) { $probe.Kill(); $probe.WaitForExit() }
Get-Content -LiteralPath $log
Write-Output "EVIDENCE $log"
if (!$finished) { Write-Output 'Probe exceeded 12 s'; exit 124 }
exit $probe.ExitCode
