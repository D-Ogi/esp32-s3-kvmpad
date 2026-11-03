# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# TASK-050 (M2) HIL check on a Windows HOST: the composite device enumerates with the three HID
# interfaces of TECH-030/ADR-018 (boot keyboard, boot mouse, Consumer Control) and every node is
# started by the in-box drivers. With -CapsLed the HOST toggles Caps Lock twice (state restored)
# and the device console must report the LED output reports.
#
# Usage: pwsh -NoProfile -File test/hil/usb_enum_windows.ps1 [-CapsLed] [-Port COM3]
# Evidence goes to ../evidence/hardware (outside the repository) with its SHA256.
param(
    [switch]$CapsLed,
    [string]$Port = 'COM3',
    [string]$Vid = '303A',
    [string]$Pid_ = '400C'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$evidence = Join-Path (Split-Path -Parent $repo) 'evidence/hardware'
New-Item -ItemType Directory -Force $evidence | Out-Null
$log = Join-Path $evidence ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-usb-enum-windows.log')
$lines = [System.Collections.Generic.List[string]]::new()
$fails = [System.Collections.Generic.List[string]]::new()
function Note([string]$s) { $lines.Add($s); Write-Output $s }

$ids = "VID_$Vid&PID_$Pid_"
$devs = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match [regex]::Escape($ids) })
foreach ($d in $devs) {
    $compat = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_CompatibleIds -ErrorAction SilentlyContinue).Data
    Note ("PNP {0} | {1} | {2} | {3}" -f $d.Status, $d.Class, $d.InstanceId, ($compat -join ';'))
}

function Expect([string]$what, [scriptblock]$pred) {
    $hit = @($devs | Where-Object $pred)
    if ($hit.Count -ne 1) { $fails.Add("$what : expected 1 node, found $($hit.Count)"); return }
    if ($hit[0].Status -ne 'OK') { $fails.Add("$what : status $($hit[0].Status)"); return }
    Note "OK  $what"
}
function Compat($d) { (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_CompatibleIds -ErrorAction SilentlyContinue).Data -join ';' }

Expect 'composite parent (USB\COMPOSITE)' { $_.InstanceId -match "^USB\\$ids\\" -and (Compat $_) -match 'USB\\COMPOSITE' }
Expect 'MI_00 boot keyboard interface (Class_03&SubClass_01&Prot_01)' { $_.InstanceId -match "^USB\\$ids&MI_00" -and (Compat $_) -match 'Class_03&SubClass_01&Prot_01' }
Expect 'MI_01 boot mouse interface (Class_03&SubClass_01&Prot_02)' { $_.InstanceId -match "^USB\\$ids&MI_01" -and (Compat $_) -match 'Class_03&SubClass_01&Prot_02' }
Expect 'MI_02 consumer interface (Class_03&SubClass_00&Prot_00)' { $_.InstanceId -match "^USB\\$ids&MI_02" -and (Compat $_) -match 'Class_03&SubClass_00&Prot_00' }
Expect 'HID keyboard collection (Keyboard class)' { $_.InstanceId -match "^HID\\$ids&MI_00" -and $_.Class -eq 'Keyboard' }
Expect 'HID mouse collection (Mouse class)' { $_.InstanceId -match "^HID\\$ids&MI_01" -and $_.Class -eq 'Mouse' }
Expect 'HID consumer collection (HIDClass)' { $_.InstanceId -match "^HID\\$ids&MI_02" -and $_.Class -eq 'HIDClass' }
$serial = @($devs | Where-Object { $_.InstanceId -match "^USB\\$ids\\" }) | ForEach-Object { ($_.InstanceId -split '\\')[-1] }
Note "SERIAL $serial"

if ($CapsLed) {
    # Toggle Caps Lock on the HOST twice: Windows sends a SET_REPORT(Output) to every keyboard.
    Add-Type -Namespace KvmHil -Name Kbd -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);'
    $py = Join-Path (Split-Path -Parent $repo) '.tools/venv/Scripts/python.exe'
    $ser = Join-Path $PSScriptRoot 'kvm_serial.py'
    $before = (& $py $ser --port $Port cmd 'usb' 2>&1 | Out-String)
    foreach ($i in 1..2) {
        [KvmHil.Kbd]::keybd_event(0x14, 0x3A, 0, [UIntPtr]::Zero)
        [KvmHil.Kbd]::keybd_event(0x14, 0x3A, 2, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 400
    }
    $after = (& $py $ser --port $Port cmd 'usb' 2>&1 | Out-String)
    $rx = 'led_reports=(\d+)'
    $b = if ($before -match $rx) { [int]$Matches[1] } else { -1 }
    $a = if ($after -match $rx) { [int]$Matches[1] } else { -1 }
    Note "CAPSLED led_reports before=$b after=$a"
    if ($b -lt 0 -or $a -lt $b + 2) { $fails.Add("Caps Lock LED: expected >= 2 new output reports, got $($a - $b)") } else { Note 'OK  Caps Lock LED output reports received' }
}

$verdict = if ($fails.Count -eq 0) { 'USB_ENUM PASS' } else { 'USB_ENUM FAIL: ' + ($fails -join ' | ') }
Note $verdict
$lines | Set-Content -Encoding utf8 $log
$sha = (Get-FileHash -Algorithm SHA256 $log).Hash.ToLower()
Write-Output "EVIDENCE $log sha256=$sha"
if ($fails.Count -ne 0) { exit 1 }
