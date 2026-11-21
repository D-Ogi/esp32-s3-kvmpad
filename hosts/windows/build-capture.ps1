# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
param([Parameter(Mandatory)][string]$OutputDirectory, [switch]$TestGeometry, [switch]$TestStudio, [switch]$TestTelemetry, [string]$SimConnectSdk)
$ErrorActionPreference = 'Stop'
$locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$install) { throw 'Install the Visual C++ x64 build tools and Windows SDK 10.0.26100.0.' }
& (Join-Path $install 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$sourcePath = Join-Path $PSScriptRoot 'native\capture.cpp'
$geometryPath = Join-Path $PSScriptRoot 'native\test_region.cpp'
$shareWindowTest = Join-Path $PSScriptRoot 'native\test_share_window.cpp'
$surfaceTest = Join-Path $PSScriptRoot 'native\test_capture_surface.cpp'
$msfsPath = Join-Path $PSScriptRoot 'native\msfs.cpp'
$studioPath = Join-Path $PSScriptRoot 'native\studio_io.cpp'
$studioTest = Join-Path $PSScriptRoot 'native\test_studio_peer.c'
$firmware = Join-Path $PSScriptRoot '..\..\components\usb_hid_device'
if ($SimConnectSdk) {
    $includeDir = (Resolve-Path (Join-Path $SimConnectSdk 'include')).Path
    $library = (Resolve-Path (Join-Path $SimConnectSdk 'lib\SimConnect.lib')).Path
}
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
Push-Location -LiteralPath $OutputDirectory
try {
    & cl /nologo /std:c++20 /EHsc /W4 /O2 /DUNICODE /D_UNICODE $sourcePath /Fe:kvmpad-capture.exe /link d3d11.lib windowsapp.lib user32.lib gdi32.lib wtsapi32.lib ole32.lib windowscodecs.lib
    if ($LASTEXITCODE -ne 0) { throw "Capture build failed ($LASTEXITCODE)" }
    & cl /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE $studioPath /Fe:kvmpad-studio-io.exe /link hid.lib setupapi.lib
    if ($LASTEXITCODE -ne 0) { throw "Studio HID build failed ($LASTEXITCODE)" }
    if ($TestGeometry) {
        & cl /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE $shareWindowTest /Fe:kvmpad-test-share-window.exe /link user32.lib gdi32.lib
        if ($LASTEXITCODE -ne 0) { throw "Share window test build failed ($LASTEXITCODE)" }
        & .\kvmpad-test-share-window.exe
        if ($LASTEXITCODE -ne 0) { throw "Share window test failed ($LASTEXITCODE)" }
        & cl /nologo /std:c++17 /EHsc /W4 /O2 $geometryPath /Fe:kvmpad-test-region.exe
        if ($LASTEXITCODE -ne 0) { throw "Geometry build failed ($LASTEXITCODE)" }
        & .\kvmpad-test-region.exe
        if ($LASTEXITCODE -ne 0) { throw "Geometry check failed ($LASTEXITCODE)" }
        & cl /nologo /std:c++20 /EHsc /W4 /O2 $surfaceTest /Fe:kvmpad-test-surface.exe /link d3d11.lib windowsapp.lib ole32.lib windowscodecs.lib
        if ($LASTEXITCODE -ne 0) { throw "Synthetic surface build failed ($LASTEXITCODE)" }
    }
    if ($TestStudio) {
        & cl /nologo /std:c11 /LD /W4 /O2 "/I$firmware/include" $studioTest "$firmware/src/hpe_studio.c" "$firmware/src/hpe_reports.c" /Fe:kvmpad-test-studio.dll
        if ($LASTEXITCODE -ne 0) { throw "Studio protocol fixture build failed ($LASTEXITCODE)" }
    }
    if ($TestTelemetry) {
        $fixtureInclude = Join-Path $PSScriptRoot 'native/test_simconnect'
        & cl /nologo /std:c++17 /EHsc /W4 /O2 /D_CRT_SECURE_NO_WARNINGS "/I$fixtureInclude" $msfsPath /Fe:kvmpad-test-simconnect.exe
        if ($LASTEXITCODE -ne 0) { throw "Offline simulator helper build failed ($LASTEXITCODE)" }
    }
    if ($SimConnectSdk) {
        & cl /nologo /std:c++17 /EHsc /W4 /O2 "/I$includeDir" $msfsPath /Fe:kvmpad-msfs.exe /link $library
        if ($LASTEXITCODE -ne 0) { throw "SimConnect build failed ($LASTEXITCODE)" }
    }
} finally { Pop-Location }
