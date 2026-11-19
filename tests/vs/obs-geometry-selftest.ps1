# Copyright (C) 2026 Jeffrey Brick
# SPDX-License-Identifier: GPL-2.0-or-later
# Attach to an already running portable OBS; never starts, closes, or kills OBS.
param(
    [string]$ObsRoot = 'C:\Users\jeffr\OBS-Studio-33-Portable',
    [string]$OutputDirectory = "$PSScriptRoot\..\build_x64\geometry-previews"
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not (Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$ObsRoot\bin\64bit\obs64.exe" })) {
    throw 'Start the intended portable OBS instance before running this attach-only test.'
}
$cfg = Get-Content -LiteralPath "$ObsRoot\config\obs-studio\plugin_config\obs-websocket\config.json" | ConvertFrom-Json
$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ct = [System.Threading.CancellationToken]::None
$suffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$sceneName = "__vs_geometry_scene_$suffix"
$inputName = "__vs_geometry_color_$suffix"
$filterName = "__vs_geometry_shader_$suffix"
$audioName = "__vs_geometry_audio_$suffix"
$sceneCreated = $false; $inputCreated = $false; $audioCreated = $false
$originalScene = ''; $originalFader = $null
$checks = [System.Collections.Generic.List[object]]::new()
$skipped = [System.Collections.Generic.List[object]]::new()
$expectedWarnings = [System.Collections.Generic.List[string]]::new()
$previewPath = [System.IO.Path]::GetFullPath((Join-Path $OutputDirectory $suffix))
$null = New-Item -ItemType Directory -Path $previewPath -Force

function Receive-Json {
    $stream = [System.IO.MemoryStream]::new(); $buffer = New-Object byte[] 65536
    try {
        do {
            $timeout = [System.Threading.CancellationTokenSource]::new(10000)
            try { $result = $ws.ReceiveAsync([System.ArraySegment[byte]]::new($buffer), $timeout.Token).GetAwaiter().GetResult() }
            finally { $timeout.Dispose() }
            if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { throw 'OBS disconnected' }
            $stream.Write($buffer, 0, $result.Count)
        } while (-not $result.EndOfMessage)
        return ([System.Text.Encoding]::UTF8.GetString($stream.ToArray()) | ConvertFrom-Json)
    } finally { $stream.Dispose() }
}
function Invoke-Obs($type, $data) {
    $id = [Guid]::NewGuid().ToString()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes((@{ op = 6; d = @{ requestType = $type; requestId = $id; requestData = $data } } | ConvertTo-Json -Depth 15 -Compress))
    $ws.SendAsync([System.ArraySegment[byte]]::new($bytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).GetAwaiter().GetResult() | Out-Null
    do { $message = Receive-Json } until ($message.op -eq 7 -and $message.d.requestId -eq $id)
    if (-not $message.d.requestStatus.result) { throw "${type}: $($message.d.requestStatus.comment)" }
    return $message.d.responseData
}
function Invoke-Fadester($type, $data) {
    $result = (Invoke-Obs 'CallVendorRequest' @{ vendorName = 'midi-fadester'; requestType = $type; requestData = $data }).responseData
    if ($result.error) { throw "Fadester ${type}: $($result.error)" }
    return $result
}
function Set-Fader([double]$value) {
    $null = Invoke-Fadester 'set_fader_value' @{ slot = 'fader_1'; value = $value }
    Start-Sleep -Milliseconds 200
}
function Set-Shader($settings) {
    $null = Invoke-Obs 'SetSourceFilterSettings' @{ sourceName = $inputName; filterName = $filterName; filterSettings = $settings; overlay = $true }
    Start-Sleep -Milliseconds 200
}
function Set-Input($settings) {
    $null = Invoke-Obs 'SetInputSettings' @{ inputName = $inputName; inputSettings = $settings; overlay = $true }
    Start-Sleep -Milliseconds 200
}
function Get-Frame([string]$name) {
    # Native-size source capture preserves portrait geometry and PNG alpha.
    $result = Invoke-Obs 'GetSourceScreenshot' @{ sourceName = $inputName; imageFormat = 'png' }
    $bytes = [Convert]::FromBase64String(($result.imageData -replace '^data:image/[^;]+;base64,', ''))
    $path = Join-Path $previewPath "$name.png"
    [System.IO.File]::WriteAllBytes($path, $bytes)
    $stream = [System.IO.MemoryStream]::new($bytes, $false)
    $bitmap = [System.Drawing.Bitmap]::new($stream)
    try {
        $rect = [System.Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height)
        $locked = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            if ($locked.Stride -le 0) { throw 'Unexpected PNG pixel layout' }
            $pixels = New-Object byte[] ($locked.Stride * $bitmap.Height)
            [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $pixels, 0, $pixels.Length)
            return [pscustomobject]@{ pixels = $pixels; width = $bitmap.Width; height = $bitmap.Height; stride = $locked.Stride; path = $path }
        } finally { $bitmap.UnlockBits($locked) }
    } finally { $bitmap.Dispose(); $stream.Dispose() }
}
function Compare-Frames($first, $second) {
    if ($first.width -ne $second.width -or $first.height -ne $second.height) { throw 'Frame dimensions differ' }
    [double]$sum = 0; $maximum = 0; $alphaMax = 0; $changed = 0; $count = 0
    # Bound analysis to roughly 320 samples on the long axis; save full native PNGs for visual inspection.
    $sampleStep = [Math]::Max(2, [int][Math]::Ceiling([Math]::Max($first.width, $first.height) / 320.0))
    for ($y = 0; $y -lt $first.height; $y += $sampleStep) {
        for ($x = 0; $x -lt $first.width; $x += $sampleStep) {
            $a = $y * $first.stride + $x * 4; $b = $y * $second.stride + $x * 4
            $pixelMax = 0
            for ($c = 0; $c -lt 3; $c++) {
                $delta = [Math]::Abs([int]$first.pixels[$a + $c] - [int]$second.pixels[$b + $c])
                $sum += $delta; $maximum = [Math]::Max($maximum, $delta); $pixelMax = [Math]::Max($pixelMax, $delta)
            }
            $alphaMax = [Math]::Max($alphaMax, [Math]::Abs([int]$first.pixels[$a + 3] - [int]$second.pixels[$b + 3]))
            if ($pixelMax -gt 3) { $changed++ }; $count++
        }
    }
    return [pscustomobject]@{ mean_rgb_difference = $sum / (3 * $count); max_rgb_difference = $maximum; max_alpha_difference = $alphaMax; changed_fraction = $changed / [double]$count; sample_step = $sampleStep; samples = $count }
}
function Check-Frames($name, $first, $second, [bool]$shouldDiffer) {
    $comparison = Compare-Frames $first $second
    if ($comparison.max_alpha_difference -gt 1) { throw "$name changed source alpha: $($comparison | ConvertTo-Json -Compress)" }
    if ($shouldDiffer) {
        if ($comparison.mean_rgb_difference -lt 0.5 -or $comparison.changed_fraction -lt 0.01) { throw "$name did not visibly change pixels: $($comparison | ConvertTo-Json -Compress)" }
    } elseif ($comparison.max_rgb_difference -gt 1) { throw "$name differs from dry source: $($comparison | ConvertTo-Json -Compress)" }
    $checks.Add([pscustomobject]@{ check = $name; result = 'PASS'; comparison = $comparison })
}
function Assert-BaselineAlpha($frame, [int]$expected) {
    for ($y = 0; $y -lt $frame.height; $y += 8) {
        for ($x = 0; $x -lt $frame.width; $x += 8) {
            if ([Math]::Abs([int]$frame.pixels[$y * $frame.stride + $x * 4 + 3] - $expected) -gt 1) {
                throw "PNG capture did not retain expected source alpha $expected"
            }
        }
    }
}
function Write-TestTone([string]$path) {
    # One-second seamless 48 kHz mono PCM loop: steady bass and treble, no microphone or output device.
    $rate = 48000; $sampleCount = $rate; $dataBytes = $sampleCount * 2
    $file = [System.IO.File]::Create($path)
    $writer = [System.IO.BinaryWriter]::new($file)
    try {
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([int](36 + $dataBytes))
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([int]16)
        $writer.Write([int16]1); $writer.Write([int16]1); $writer.Write([int]$rate); $writer.Write([int]($rate * 2))
        $writer.Write([int16]2); $writer.Write([int16]16)
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([int]$dataBytes)
        for ($i = 0; $i -lt $sampleCount; $i++) {
            $sample = 0.24 * ([Math]::Sin(2 * [Math]::PI * 80 * $i / $rate) + [Math]::Sin(2 * [Math]::PI * 6000 * $i / $rate))
            $writer.Write([int16][Math]::Round(32767 * $sample))
        }
    } finally { $writer.Dispose(); $file.Dispose() }
}

try {
    $ws.ConnectAsync([Uri]"ws://127.0.0.1:$($cfg.server_port)", $ct).GetAwaiter().GetResult() | Out-Null
    $hello = Receive-Json; $identify = @{ rpcVersion = 1; eventSubscriptions = 0 }
    if ($hello.d.authentication) {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $secret = [Convert]::ToBase64String($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($cfg.server_password + $hello.d.authentication.salt)))
            $identify.authentication = [Convert]::ToBase64String($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($secret + $hello.d.authentication.challenge)))
        } finally { $sha.Dispose() }
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes((@{ op = 1; d = $identify } | ConvertTo-Json -Compress))
    $ws.SendAsync([System.ArraySegment[byte]]::new($bytes), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).GetAwaiter().GetResult() | Out-Null
    if ((Receive-Json).op -ne 2) { throw 'OBS authentication failed' }
    if ((Invoke-Obs 'GetStreamStatus' @{}).outputActive -or (Invoke-Obs 'GetRecordStatus' @{}).outputActive -or (Invoke-Obs 'GetVirtualCamStatus' @{}).outputActive) { throw 'Stop: OBS has an active output' }
    $hotkeys = @((Invoke-Obs 'GetHotkeyList' @{}).hotkeys)
    $panicRegistered = $hotkeys -contains 'visual_sidechain_panic'
    $restoreRegistered = $hotkeys -contains 'visual_sidechain_restore'
    # Hotkeys affect every plugin filter, and get_signals does not expose the prior global panic state.
    # Discover their names but do not toggle an operator's potentially active panic state blindly.
    $originalScene = (Invoke-Obs 'GetCurrentProgramScene' @{}).currentProgramSceneName
    $slots = (Invoke-Fadester 'get_state' @{}).slots
    foreach ($slot in $slots) { if ($slot.id -eq 'fader_1') { $originalFader = [double]$slot.value } }
    if ($null -eq $originalFader) { throw 'Fadester slot fader_1 is required' }
    $null = Invoke-Obs 'CreateScene' @{ sceneName = $sceneName }; $sceneCreated = $true
    $null = Invoke-Obs 'CreateInput' @{ sceneName = $sceneName; inputName = $inputName; inputKind = 'color_source_v3'; inputSettings = @{ color = 4280295456; width = 640; height = 360 }; sceneItemEnabled = $true }; $inputCreated = $true
    $null = Invoke-Obs 'SetCurrentProgramScene' @{ sceneName = $sceneName }; Start-Sleep -Milliseconds 300
    $baseline = Get-Frame 'dry-landscape'
    # Configure an EMPTY media source first: audio must never reach a mixer track or monitoring device.
    $null = Invoke-Obs 'CreateInput' @{ sceneName = $sceneName; inputName = $audioName; inputKind = 'ffmpeg_source'; inputSettings = @{ is_local_file = $true; local_file = '' }; sceneItemEnabled = $true }; $audioCreated = $true
    $null = Invoke-Obs 'SetInputAudioTracks' @{ inputName = $audioName; inputAudioTracks = @{ '1' = $false; '2' = $false; '3' = $false; '4' = $false; '5' = $false; '6' = $false } }
    $null = Invoke-Obs 'SetInputAudioMonitorType' @{ inputName = $audioName; monitorType = 'OBS_MONITORING_TYPE_NONE' }
    $null = Invoke-Obs 'SetInputMute' @{ inputName = $audioName; inputMuted = $true }
    $tonePath = Join-Path $previewPath 'bass-treble-test.wav'; Write-TestTone $tonePath
    $null = Invoke-Obs 'SetInputSettings' @{ inputName = $audioName; inputSettings = @{ is_local_file = $true; local_file = $tonePath; looping = $true; restart_on_activate = $true }; overlay = $true }
    Start-Sleep -Milliseconds 1000
    $presets = @(
        @{ name = 'hex'; file = 'fft-hex-pulse-lattice.shader'; opacity = 'hex_opacity'; audio = 'hex_audio_amount'; target = 'hex_cells'; min = 4.0; max = 32.0; static = @{ hex_ripple_speed = 0.0; hex_audio_amount = 0.0; hex_opacity = 0.8 }; extremes = @{ hex_cells = 32.0; hex_line_width = 0.01 } },
        @{ name = 'tunnel'; file = 'fft-polygon-tunnel.shader'; opacity = 'tunnel_opacity'; audio = 'tunnel_audio_amount'; target = 'tunnel_depth'; min = 2.0; max = 8.0; static = @{ tunnel_speed = 0.0; tunnel_rotation_speed = 0.0; tunnel_audio_amount = 0.0; tunnel_opacity = 0.8 }; extremes = @{ tunnel_sides = 3; tunnel_depth = 8.0; tunnel_line_width = 0.01 } }
    )
    foreach ($preset in $presets) {
        $settings = @{ preset_shader = $preset.file; mix_fader_slot = -1; reactivity_fader_slot = -1; tap_mode = 7; tap_source_name = $audioName; react_when_muted = $true }
        foreach ($key in $preset.static.Keys) { $settings[$key] = $preset.static[$key] }
        $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = $filterName; filterKind = 'visual_sidechain_shader_filter'; filterSettings = $settings }
        Start-Sleep -Milliseconds 300
        Check-Frames "$($preset.name)_pattern" $baseline (Get-Frame "$($preset.name)-landscape") $true
        $skipped.Add([pscustomobject]@{ check = "$($preset.name)_panic_restore"; result = 'SKIP'; reason = 'No panic-state getter exists; global hotkeys would mutate filters outside this fixture.'; panic_registered = $panicRegistered; restore_registered = $restoreRegistered })
        $missingPreset = "__vs_missing_${suffix}_$($preset.name).shader"
        $expectedWarnings.Add("No shader file path specified. (intentional unavailable bundled preset: $missingPreset)")
        Set-Shader @{ preset_shader = $missingPreset }
        Check-Frames "$($preset.name)_missing_preset_dry_fallback" $baseline (Get-Frame "$($preset.name)-missing-preset") $false
        Set-Shader @{ preset_shader = $preset.file }
        Check-Frames "$($preset.name)_missing_preset_recovery" $baseline (Get-Frame "$($preset.name)-recovered") $true
        $audioOff = Get-Frame "$($preset.name)-audio-off"
        Set-Shader @{ $preset.audio = 1.0; reactivity_fader_slot = 1 }; Set-Fader 0.0
        Check-Frames "$($preset.name)_reactivity_zero" $audioOff (Get-Frame "$($preset.name)-reactivity-zero") $false
        Set-Fader 1.0; Start-Sleep -Milliseconds 1000
        Check-Frames "$($preset.name)_controlled_audio" $audioOff (Get-Frame "$($preset.name)-audio-on") $true
        Set-Shader @{ $preset.audio = 0.0; reactivity_fader_slot = -1 }
        Set-Shader @{ $preset.opacity = 0.0 }
        Check-Frames "$($preset.name)_zero_opacity" $baseline (Get-Frame "$($preset.name)-zero-opacity") $false
        Set-Shader @{ $preset.opacity = 0.8; mix_fader_slot = 1 }; Set-Fader 0.0
        Check-Frames "$($preset.name)_mix_dry" $baseline (Get-Frame "$($preset.name)-mix-dry") $false
        Set-Shader @{ mix_fader_slot = -1; param_fader_1_target = $preset.target; param_fader_1_slot = 1; param_fader_1_min = $preset.min; param_fader_1_max = $preset.max }
        $minimum = Get-Frame "$($preset.name)-fader-min"
        Set-Fader 1.0; $maximum = Get-Frame "$($preset.name)-fader-max"
        Check-Frames "$($preset.name)_geometry_fader" $minimum $maximum $true
        Set-Shader @{ param_fader_1_target = ''; param_fader_1_slot = -1 }
        Set-Shader $preset.extremes
        Check-Frames "$($preset.name)_extreme_geometry" $baseline (Get-Frame "$($preset.name)-extreme") $true
        if ($preset.name -eq 'tunnel') {
            $triangle = Get-Frame 'tunnel-triangle'
            Set-Shader @{ tunnel_sides = 8 }
            Check-Frames 'tunnel_polygon_sides' $triangle (Get-Frame 'tunnel-octagon') $true
        }
        foreach ($resolution in @(@{ name = '1080p'; width = 1920; height = 1080 }, @{ name = '4k'; width = 3840; height = 2160 })) {
            $null = Invoke-Obs 'SetSourceFilterEnabled' @{ sourceName = $inputName; filterName = $filterName; filterEnabled = $false }
            Set-Input @{ width = $resolution.width; height = $resolution.height }
            $largeDry = Get-Frame "$($preset.name)-dry-$($resolution.name)"
            if ($largeDry.width -ne $resolution.width -or $largeDry.height -ne $resolution.height) { throw 'OBS did not capture the requested native resolution' }
            $null = Invoke-Obs 'SetSourceFilterEnabled' @{ sourceName = $inputName; filterName = $filterName; filterEnabled = $true }; Start-Sleep -Milliseconds 200
            Check-Frames "$($preset.name)_$($resolution.name)_pattern" $largeDry (Get-Frame "$($preset.name)-$($resolution.name)") $true
        }
        $null = Invoke-Obs 'SetSourceFilterEnabled' @{ sourceName = $inputName; filterName = $filterName; filterEnabled = $false }
        Set-Input @{ width = 360; height = 640 }; $portrait = Get-Frame 'dry-portrait'
        $null = Invoke-Obs 'SetSourceFilterEnabled' @{ sourceName = $inputName; filterName = $filterName; filterEnabled = $true }; Start-Sleep -Milliseconds 200
        Check-Frames "$($preset.name)_portrait" $portrait (Get-Frame "$($preset.name)-portrait") $true
        foreach ($colorCase in @(@{ name = 'transparent'; color = 0; alpha = 0; differs = $false }, @{ name = 'half-alpha'; color = 2149589024; alpha = 128; differs = $true })) {
            $null = Invoke-Obs 'SetSourceFilterEnabled' @{ sourceName = $inputName; filterName = $filterName; filterEnabled = $false }
            Set-Input @{ color = $colorCase.color; width = 640; height = 360 }; $alphaBaseline = Get-Frame "dry-$($colorCase.name)"
            Assert-BaselineAlpha $alphaBaseline $colorCase.alpha
            $null = Invoke-Obs 'SetSourceFilterEnabled' @{ sourceName = $inputName; filterName = $filterName; filterEnabled = $true }; Start-Sleep -Milliseconds 200
            Check-Frames "$($preset.name)_$($colorCase.name)" $alphaBaseline (Get-Frame "$($preset.name)-$($colorCase.name)") $colorCase.differs
        }
        $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = $filterName }
        Set-Input @{ color = 4280295456; width = 640; height = 360 }
    }
    [pscustomobject]@{ passed_count = $checks.Count; skipped_count = $skipped.Count; checks = $checks; skipped = $skipped; expected_warnings = $expectedWarnings; screenshots = $previewPath; restored_scene = $originalScene; limitations = 'SDR screenshot checks up to native 3840x2160, with sparse pixel analysis; not a sustained 4K performance test. Inspect PNGs for geometry and aliasing. Pixels cannot prove absence of intermediate shader NaNs. Global Panic/Restore is not exercised because prior panic state cannot be read safely.' } | ConvertTo-Json -Depth 8
    if ($checks.Count -ne 29 -or $skipped.Count -ne 2) { throw "Expected 29 geometry checks and 2 declared skips; got $($checks.Count) checks and $($skipped.Count) skips" }
} finally {
    if ($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
        try { if ($originalScene) { $null = Invoke-Obs 'SetCurrentProgramScene' @{ sceneName = $originalScene } } } catch { Write-Warning $_ }
        try { if ($null -ne $originalFader) { $null = Invoke-Fadester 'set_fader_value' @{ slot = 'fader_1'; value = $originalFader } } } catch { Write-Warning $_ }
        try { if ($audioCreated) { $null = Invoke-Obs 'RemoveInput' @{ inputName = $audioName } } } catch { Write-Warning $_ }
        try { if ($inputCreated) { $null = Invoke-Obs 'RemoveInput' @{ inputName = $inputName } } } catch { Write-Warning $_ }
        try { if ($sceneCreated) { $null = Invoke-Obs 'RemoveScene' @{ sceneName = $sceneName } } } catch { Write-Warning $_ }
        $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, 'geometry audit complete', $ct).GetAwaiter().GetResult() | Out-Null
    }
    $ws.Dispose()
}
