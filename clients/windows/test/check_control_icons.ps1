# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
# Exercise the packaged WinForms renderer without opening a window or changing Windows settings.
param([Parameter(Mandatory)][string]$AppDirectory)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$app = (Resolve-Path -LiteralPath $AppDirectory).Path
$source = Join-Path $PSScriptRoot '../../../design/assets/control-icons'
$assets = [IO.Path]::Combine($app, 'assets', 'control-icons')
$names = 'mouse','keyboard','shortcuts','extensions','host','client','connect','disconnect',
         'magnifier','video','card','settings','day','night','contrast','home'
foreach ($name in $names) {
    if ((Get-FileHash (Join-Path $source "$name.png")).Hash -ne
        (Get-FileHash (Join-Path $assets "$name.png")).Hash) { throw "Packaging changed $name." }
}

$dll = [Reflection.Assembly]::LoadFrom((Join-Path $app 'KVMpad.Client.dll'))
$icons = $dll.GetType('KVMpad.Client.App.ControlIcons', $true).GetConstructor([type[]]@([string])).Invoke(@($assets))
$roleType = $dll.GetType('KVMpad.Client.App.IconRole', $true)
$buttons = @{}
$roles = @{connect='Client'; disconnect='Client'; mouse='Client'; shortcuts='Client'; keyboard='Client';
           host='Host'; settings='Neutral'; extensions='Neutral'; video='Neutral'; missing='Neutral'}
$contrastInk = [Drawing.Color]::Yellow
try {
    foreach ($name in $roles.Keys) {
        $button = [Windows.Forms.Button]::new()
        $button.Text = $name
        $buttons[$name] = $button
        $icons.Add($button, $name, [Enum]::Parse($roleType, $roles[$name]))
    }
    foreach ($dpi in 96,144,192) {
        foreach ($mode in 'day','night','contrast') {
            $icons.Apply($dpi, $mode -eq 'night', $mode -eq 'contrast', $contrastInk)
            $size = 20 * $dpi / 96
            foreach ($name in $roles.Keys) {
                $button = $buttons[$name]
                if ($button.Text -ne $name -or $button.TextImageRelation -ne 'ImageBeforeText') {
                    throw "Missing label for $name."
                }
                $image = $button.Image
                if ($name -eq 'missing') {
                    if ($null -ne $image) { throw 'A missing asset should retain only its text.' }
                    continue
                }
                if ($image.Width -ne $size -or $image.Height -ne $size) { throw "Wrong $name size at $dpi DPI." }
                $expected = if ($mode -eq 'contrast') { $contrastInk }
                    elseif ($mode -eq 'day') { [Drawing.ColorTranslator]::FromHtml(@{
                        Neutral='#1D1D1F'; Client='#006ACB'; Host='#21724D'}[$roles[$name]]) }
                    else { [Drawing.ColorTranslator]::FromHtml(@{
                        Neutral='#E5E7EB'; Client='#84BFFF'; Host='#77D6AE'}[$roles[$name]]) }
                $clear = $opaque = 0
                for ($y=0; $y -lt $size; $y++) {
                    for ($x=0; $x -lt $size; $x++) {
                        $pixel = $image.GetPixel($x,$y)
                        if ($pixel.A -eq 0) { $clear++ }
                        if ($pixel.A -ge 200) {
                            $opaque++
                            if ([Math]::Abs($pixel.R - $expected.R) -gt 2 -or
                                [Math]::Abs($pixel.G - $expected.G) -gt 2 -or
                                [Math]::Abs($pixel.B - $expected.B) -gt 2) { throw "Wrong $name color in $mode." }
                        }
                    }
                }
                if ($opaque -lt 3 -or $clear -lt 8) { throw "Unreadable $name mask at $dpi DPI." }
            }
            $old = $buttons['host'].Image
            $icons.Apply($dpi, $mode -eq 'night', $mode -eq 'contrast', $contrastInk)
            if (![object]::ReferenceEquals($old, $buttons['host'].Image)) { throw 'A redraw replaced a cached icon.' }
        }
    }
    Write-Output 'PASS: 16 packaged icons; 9 WinForms actions in day, night and system contrast at 96/144/192 DPI; missing asset keeps its label.'
}
finally {
    $icons.Dispose()
    foreach ($button in $buttons.Values) { $button.Dispose() }
}
