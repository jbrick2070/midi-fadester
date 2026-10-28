# Copyright (C) 2026 Jeffrey Brick
# SPDX-License-Identifier: GPL-2.0-or-later
# Attach to an already running portable OBS. Never starts, closes, or kills OBS.
# Creates a temporary scene/source, restores the operator's scene/faders, and removes only its own objects.
param(
    [string]$ObsRoot = 'C:\Users\jeffr\OBS-Studio-33-Portable',
    [ValidateRange(0, 300)][int]$StressSeconds = 30
)
$ErrorActionPreference = 'Stop'
$portableObs = Get-Process obs64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$ObsRoot\bin\64bit\obs64.exe" } | Select-Object -First 1
if (-not $portableObs) {
    throw 'Start the intended portable OBS instance before running this attach-only test.'
}
$cfg = Get-Content -LiteralPath "$ObsRoot\config\obs-studio\plugin_config\obs-websocket\config.json" | ConvertFrom-Json
$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ct = [System.Threading.CancellationToken]::None
$suffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$sceneName = "__vs_audit_scene_$suffix"
$inputName = "__vs_audit_color_$suffix"
$filterName = "__vs_audit_shader_$suffix"
$audioName = "__vs_audit_audio_$suffix"
$sceneCreated = $false
$inputCreated = $false
$audioCreated = $false
$originalScene = ''
$originalFaders = @{}
$checks = [System.Collections.Generic.List[object]]::new()
$expectedCheckCount = 26
$expectedWarnings = [System.Collections.Generic.List[string]]::new()

function Receive-Json {
    $stream = [System.IO.MemoryStream]::new()
    $buffer = New-Object byte[] 65536
    try {
        do {
            $segment = [System.ArraySegment[byte]]::new($buffer, 0, $buffer.Length)
            $timeout = [System.Threading.CancellationTokenSource]::new(10000)
            try { $result = $ws.ReceiveAsync($segment, $timeout.Token).GetAwaiter().GetResult() }
            finally { $timeout.Dispose() }
            if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { throw 'OBS disconnected' }
            $stream.Write($buffer, 0, $result.Count)
        } while (-not $result.EndOfMessage)
        return ([System.Text.Encoding]::UTF8.GetString($stream.ToArray()) | ConvertFrom-Json)
    } finally { $stream.Dispose() }
}

function Send-Json($object) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes(($object | ConvertTo-Json -Depth 15 -Compress))
    $segment = [System.ArraySegment[byte]]::new($bytes, 0, $bytes.Length)
    $ws.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).GetAwaiter().GetResult() | Out-Null
}

function Invoke-Obs($type, $data) {
    $id = [Guid]::NewGuid().ToString()
    Send-Json @{ op = 6; d = @{ requestType = $type; requestId = $id; requestData = $data } }
    do { $message = Receive-Json } until ($message.op -eq 7 -and $message.d.requestId -eq $id)
    if (-not $message.d.requestStatus.result) { throw "${type}: $($message.d.requestStatus.comment)" }
    return $message.d.responseData
}

function Invoke-Fadester($type, $data) {
    $result = (Invoke-Obs 'CallVendorRequest' @{ vendorName = 'midi-fadester'; requestType = $type; requestData = $data }).responseData
    if ($result.error) { throw "Fadester ${type}: $($result.error)" }
    return $result
}

function Set-Fader($slot, [double]$value) {
    $null = Invoke-Fadester 'set_fader_value' @{ slot = $slot; value = $value }
    Start-Sleep -Milliseconds 180
}

function Set-Shader($settings) {
    $null = Invoke-Obs 'SetSourceFilterSettings' @{ sourceName = $inputName; filterName = $filterName; filterSettings = $settings; overlay = $true }
    Start-Sleep -Milliseconds 180
}

function Get-Pixel {
    # Capture the source, rather than its scene: a small scene item occupies only part of the scene canvas.
    $result = Invoke-Obs 'GetSourceScreenshot' @{ sourceName = $inputName; imageFormat = 'bmp'; imageWidth = 16; imageHeight = 16 }
    $bytes = [Convert]::FromBase64String(($result.imageData -replace '^data:image/[^;]+;base64,', ''))
    $offset = [BitConverter]::ToInt32($bytes, 10)
    $width = [BitConverter]::ToInt32($bytes, 18)
    $height = [Math]::Abs([BitConverter]::ToInt32($bytes, 22))
    $pixelBytes = [BitConverter]::ToInt16($bytes, 28) / 8
    if ($width -lt 1 -or $height -lt 1 -or $pixelBytes -lt 3) { throw 'Invalid screenshot geometry' }
    $stride = [int]([Math]::Floor(($width * $pixelBytes + 3) / 4) * 4)
    $rgb = @(0.0, 0.0, 0.0)
    for ($y = 0; $y -lt $height; $y++) {
        for ($x = 0; $x -lt $width; $x++) {
            $i = $offset + $y * $stride + $x * $pixelBytes
            $rgb[0] += $bytes[$i + 2] / 255.0
            $rgb[1] += $bytes[$i + 1] / 255.0
            $rgb[2] += $bytes[$i] / 255.0
        }
    }
    return ,@($rgb | ForEach-Object { [Math]::Round($_ / ($width * $height), 4) })
}

function Assert-Pixel($name, $expected, [double]$tolerance = 0.025) {
    $actual = Get-Pixel
    for ($i = 0; $i -lt 3; $i++) {
        if ([Math]::Abs($actual[$i] - $expected[$i]) -gt $tolerance) { throw "$name pixel mismatch: actual=$actual expected=$expected" }
    }
    $checks.Add([pscustomobject]@{ check = $name; result = 'PASS'; rgb = $actual })
}

try {
    $ws.ConnectAsync([Uri]"ws://127.0.0.1:$($cfg.server_port)", $ct).GetAwaiter().GetResult() | Out-Null
    $hello = Receive-Json
    $identify = @{ rpcVersion = 1; eventSubscriptions = 0 }
    if ($hello.d.authentication) {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $secret = [Convert]::ToBase64String($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($cfg.server_password + $hello.d.authentication.salt)))
            $identify.authentication = [Convert]::ToBase64String($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($secret + $hello.d.authentication.challenge)))
        } finally { $sha.Dispose() }
    }
    Send-Json @{ op = 1; d = $identify }
    if ((Receive-Json).op -ne 2) { throw 'OBS authentication failed' }
    if ((Invoke-Obs 'GetStreamStatus' @{}).outputActive -or (Invoke-Obs 'GetRecordStatus' @{}).outputActive -or
        (Invoke-Obs 'GetVirtualCamStatus' @{}).outputActive) { throw 'Stop: OBS has an active output' }
    $originalScene = (Invoke-Obs 'GetCurrentProgramScene' @{}).currentProgramSceneName
    $fadester = Invoke-Fadester 'get_state' @{}
    foreach ($slot in $fadester.slots) {
        if ($slot.id -in @('fader_1', 'fader_2')) { $originalFaders[$slot.id] = [double]$slot.value }
    }
    if ($originalFaders.Count -ne 2) { throw 'Fadester slots fader_1 and fader_2 are required' }
    $null = Invoke-Obs 'CreateScene' @{ sceneName = $sceneName }; $sceneCreated = $true
    $null = Invoke-Obs 'CreateInput' @{ sceneName = $sceneName; inputName = $inputName; inputKind = 'color_source_v3'; inputSettings = @{ color = 4278190335; width = 1920; height = 1080 }; sceneItemEnabled = $true }; $inputCreated = $true
    $invert = 'float4 mainImage(VertData v_in) : TARGET { float4 c = image.Sample(texture_sampler, v_in.uv); return float4(1.0-c.rgb,c.a); }'
    $collisionShader = 'uniform float effect_mix < float minimum = 0.0; float maximum = 1.0; > = 0.25; uniform float audio_reactivity < float minimum = 0.0; float maximum = 1.0; > = 0.75; float4 mainImage(VertData v_in) : TARGET { return float4(effect_mix, audio_reactivity, 0.0, 1.0); }'
    # Explicit private host keys win over conflicting legacy percentages on both creation and update.
    # Green encodes reactivity; red encodes the remaining dry mix, so pixels independently verify both controls.
    $precedenceShader = 'float4 mainImage(VertData v_in) : TARGET { return float4(0.0, vs_reactivity_scale, 0.0, 1.0); }'
    $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = $filterName; filterKind = 'visual_sidechain_shader_filter'; filterSettings = @{ preset_shader = '__custom_text__'; shader_text = $precedenceShader; effect_mix = 90.0; audio_reactivity = 10.0; vs_effect_mix_pct = 50.0; vs_audio_reactivity_pct = 25.0 } }
    $null = Invoke-Obs 'SetCurrentProgramScene' @{ sceneName = $sceneName }
    Start-Sleep -Milliseconds 500
    $precedenceObservations = [System.Collections.Generic.List[object]]::new()
    foreach ($phase in @(@{ name = 'create'; mix = 50.0; react = 25.0; rgb = @(0.7354, 0.3886, 0.0) }, @{ name = 'update'; mix = 25.0; react = 75.0; rgb = @(0.8808, 0.4702, 0.0) })) {
        if ($phase.name -eq 'update') { Set-Shader @{ effect_mix = 5.0; audio_reactivity = 95.0; vs_effect_mix_pct = $phase.mix; vs_audio_reactivity_pct = $phase.react } }
        $saved = (Invoke-Obs 'GetSourceFilter' @{ sourceName = $inputName; filterName = $filterName }).filterSettings
        if ($saved.vs_effect_mix_pct -ne $phase.mix -or $saved.vs_audio_reactivity_pct -ne $phase.react) { throw "Explicit host keys lost precedence during $($phase.name)" }
        $actual = Get-Pixel
        for ($channel = 0; $channel -lt 3; $channel++) {
            if ([Math]::Abs($actual[$channel] - $phase.rgb[$channel]) -gt 0.025) { throw "Explicit host key precedence $($phase.name) pixel mismatch: actual=$actual expected=$($phase.rgb)" }
        }
        $precedenceObservations.Add([pscustomobject]@{ phase = $phase.name; rgb = $actual; mix_pct = $saved.vs_effect_mix_pct; reactivity_pct = $saved.vs_audio_reactivity_pct })
    }
    $checks.Add([pscustomobject]@{ check = 'explicit_host_keys_take_precedence'; result = 'PASS'; phases = $precedenceObservations })
    $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = $filterName }
    # A parse-valid shader can still fail GPU compilation. Do not consume ambiguous legacy values
    # until it compiles: the repaired shader may declare those names as its own user parameters.
    $invalidShader = "float4 mainImage(VertData v_in) : TARGET { return __vs_audit_unresolved_$suffix(v_in.uv); }"
    $expectedWarnings.Add("HLSL compiler error for intentional unresolved function __vs_audit_unresolved_$suffix")
    $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = $filterName; filterKind = 'visual_sidechain_shader_filter'; filterSettings = @{ preset_shader = '__custom_text__'; shader_text = $invalidShader; effect_mix = 0.75; audio_reactivity = 0.25 } }
    Start-Sleep -Milliseconds 250
    $failedSettings = (Invoke-Obs 'GetSourceFilter' @{ sourceName = $inputName; filterName = $filterName }).filterSettings
    if ($failedSettings.effect_mix -ne 0.75 -or $failedSettings.audio_reactivity -ne 0.25) { throw 'Failed GPU compile consumed legacy parameter values' }
    foreach ($key in @('vs_effect_mix_pct', 'vs_audio_reactivity_pct')) {
        if ($null -ne $failedSettings.$key -and $failedSettings.$key -ne 100.0) { throw "Failed GPU compile migrated legacy value into $key" }
    }
    $failedPixel = Get-Pixel
    for ($channel = 0; $channel -lt 3; $channel++) {
        if ([Math]::Abs($failedPixel[$channel] - @(1.0, 0.0, 0.0)[$channel]) -gt 0.025) { throw "Failed GPU compile did not render dry: $failedPixel" }
    }
    Set-Shader @{ shader_text = $collisionShader }
    $repairedSettings = (Invoke-Obs 'GetSourceFilter' @{ sourceName = $inputName; filterName = $filterName }).filterSettings
    if ($repairedSettings.effect_mix -ne 0.75 -or $repairedSettings.audio_reactivity -ne 0.25 -or
        $repairedSettings.vs_effect_mix_pct -ne 100.0 -or $repairedSettings.vs_audio_reactivity_pct -ne 100.0) { throw 'Repair did not preserve shader-owned values and default host controls' }
    $repairedPixel = Get-Pixel
    for ($channel = 0; $channel -lt 3; $channel++) {
        if ([Math]::Abs($repairedPixel[$channel] - @(0.8808, 0.5371, 0.0)[$channel]) -gt 0.025) { throw "Repaired shader did not render saved user values: $repairedPixel" }
    }
    $checks.Add([pscustomobject]@{ check = 'failed_gpu_compile_preserves_legacy_values_until_repair'; result = 'PASS'; failed_rgb = $failedPixel; repaired_rgb = $repairedPixel; mix_pct = $repairedSettings.vs_effect_mix_pct; reactivity_pct = $repairedSettings.vs_audio_reactivity_pct })
    $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = $filterName }
    $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = $filterName; filterKind = 'visual_sidechain_shader_filter'; filterSettings = @{ preset_shader = '__custom_text__'; shader_text = $invert; effect_mix = 50.0; audio_reactivity = 25.0 } }
    $null = Invoke-Obs 'SetCurrentProgramScene' @{ sceneName = $sceneName }
    Start-Sleep -Milliseconds 500
    Assert-Pixel 'legacy_host_mix_migrates' @(0.7354, 0.7354, 0.7354)
    $migrated = (Invoke-Obs 'GetSourceFilter' @{ sourceName = $inputName; filterName = $filterName }).filterSettings
    if ($migrated.vs_effect_mix_pct -ne 50.0 -or $migrated.vs_audio_reactivity_pct -ne 25.0) { throw 'Legacy host controls did not migrate to private settings keys' }
    $checks.Add(@{ check = 'legacy_host_settings_migrate'; result = 'PASS' })
    Set-Shader @{ shader_text = $collisionShader }
    Assert-Pixel 'legacy_migration_does_not_seed_custom_uniforms' @(0.8124, 0.6461, 0.0)
    Set-Shader @{ shader_text = $invert; vs_effect_mix_pct = 100.0; vs_audio_reactivity_pct = 100.0; mix_fader_slot = 1 }
    Set-Fader 'fader_1' 0.0; Assert-Pixel 'mix_dry' @(1.0, 0.0, 0.0)
    Set-Fader 'fader_1' 1.0; Assert-Pixel 'mix_wet' @(0.0, 1.0, 1.0)
    Set-Fader 'fader_1' 0.5; Assert-Pixel 'mix_half_linear_light' @(0.7354, 0.7354, 0.7354)
    # Static Effect Mix slider works with no fader bound, and a bound fader scales the slider rather than replacing it.
    Set-Shader @{ mix_fader_slot = -1; vs_effect_mix_pct = 50.0 }; Assert-Pixel 'static_mix_half' @(0.7354, 0.7354, 0.7354)
    Set-Shader @{ vs_effect_mix_pct = 0.0 }; Assert-Pixel 'static_mix_dry' @(1.0, 0.0, 0.0)
    Set-Shader @{ vs_effect_mix_pct = 50.0; mix_fader_slot = 1 }; Set-Fader 'fader_1' 1.0; Assert-Pixel 'fader_scales_mix_slider' @(0.7354, 0.7354, 0.7354)
    $reactShader = 'float4 mainImage(VertData v_in) : TARGET { return float4(vs_reactivity_scale + audio_bass * 0.0, 0.0, 0.0, 1.0); }'
    Set-Shader @{ shader_text = $reactShader; vs_effect_mix_pct = 100.0; mix_fader_slot = -1; vs_audio_reactivity_pct = 25.0 }; Assert-Pixel 'static_reactivity_quarter' @(0.5371, 0.0, 0.0)
    Set-Shader @{ reactivity_fader_slot = 1 }; Set-Fader 'fader_1' 0.5; Assert-Pixel 'fader_scales_reactivity_slider' @(0.3884, 0.0, 0.0)
    $paramShader = 'uniform float probe < float minimum = 0.0; float maximum = 2.0; > = 0.0; float4 mainImage(VertData v_in) : TARGET { return float4(probe / 2.0,0.0,0.0,1.0); }'
    Set-Shader @{ shader_text = $paramShader; mix_fader_slot = -1; reactivity_fader_slot = -1; vs_audio_reactivity_pct = 100.0; param_fader_1_target = 'probe'; param_fader_1_slot = 1; param_fader_1_min = 0.0; param_fader_1_max = 2.0 }
    Set-Fader 'fader_1' 1.0; Assert-Pixel 'parameter_fader_max' @(1.0, 0.0, 0.0)
    Set-Fader 'fader_1' 0.0; Assert-Pixel 'parameter_fader_min' @(0.0, 0.0, 0.0)
    $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = $filterName }
    $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = $filterName; filterKind = 'visual_sidechain_shader_filter'; filterSettings = @{ preset_shader = '__custom_text__'; shader_text = $paramShader; param_fader_1_target = 'probe'; param_fader_1_slot = 1; param_fader_1_seeded = '__custom_text__::probe' } }
    Set-Fader 'fader_1' 1.0; Assert-Pixel 'parameter_seed_reload' @(1.0, 0.0, 0.0)
    # Common user uniform names must stay separate from host percent sliders, including after a preset switch.
    $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = $filterName }
    $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = $filterName; filterKind = 'visual_sidechain_shader_filter'; filterSettings = @{ preset_shader = '__custom_text__'; shader_text = $collisionShader } }
    Start-Sleep -Milliseconds 250
    Set-Shader @{ custom_low_hz = 65.0 } # A subsequent update must not reinterpret uniform defaults as percentages.
    Assert-Pixel 'custom_host_name_defaults' @(0.5371, 0.8808, 0.0)
    Set-Shader @{ effect_mix = 0.75; audio_reactivity = 0.25 }
    Assert-Pixel 'custom_host_names_edit_independently' @(0.8808, 0.5371, 0.0)
    Set-Shader @{ shader_text = $invert }
    Assert-Pixel 'custom_values_not_migrated_after_switch' @(0.0, 1.0, 1.0)
    $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = $filterName }
    $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = $filterName; filterKind = 'visual_sidechain_shader_filter'; filterSettings = @{ preset_shader = '__custom_text__'; shader_text = $collisionShader; effect_mix = 0.25; audio_reactivity = 0.75 } }
    Start-Sleep -Milliseconds 250
    Assert-Pixel 'custom_saved_values_not_migrated' @(0.5371, 0.8808, 0.0)
    Set-Shader @{ vs_effect_mix_pct = 50.0 }
    Assert-Pixel 'host_mix_preserves_custom_uniforms' @(0.8124, 0.6461, 0.0)
    # Constant Fader 1 drives opacity; Fader 2 scales its depth. Check settings, then baseline restore.
    $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = 'Audit Color'; filterKind = 'color_filter_v2'; filterSettings = @{ opacity = 0.6 } }
    $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = 'Audit Modulator'; filterKind = 'visual_sidechain_modulator_filter'; filterSettings = @{ slot_1_enabled = $true; slot_1_target_filter = 'Audit Color'; slot_1_target_setting = 'opacity'; slot_1_signal = 201; slot_1_depth_fader = 2; slot_1_amount_pct = 100.0; slot_1_min_value = 0.0; slot_1_max_value = 1.0 } }
    Set-Fader 'fader_1' 0.8; Set-Fader 'fader_2' 0.25
    $opacity = (Invoke-Obs 'GetSourceFilter' @{ sourceName = $inputName; filterName = 'Audit Color' }).filterSettings.opacity
    if ([Math]::Abs($opacity - 0.2) -gt 0.01) { throw "Depth fader expected opacity 0.2, got $opacity" }
    $checks.Add(@{ check = 'modulator_depth'; result = 'PASS'; opacity = $opacity })
    $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = 'Audit Color' }
    Start-Sleep -Milliseconds 250
    $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = 'Audit Color'; filterKind = 'color_filter_v2'; filterSettings = @{ opacity = 0.6 } }
    Start-Sleep -Milliseconds 250
    $opacity = (Invoke-Obs 'GetSourceFilter' @{ sourceName = $inputName; filterName = 'Audit Color' }).filterSettings.opacity
    $checks.Add(@{ check = 'recreated_target_retry'; result = if ([Math]::Abs($opacity - 0.2) -le 0.01) { 'PASS' } else { 'FAIL' }; opacity = $opacity })
    $null = Invoke-Obs 'SetSourceFilterEnabled' @{ sourceName = $inputName; filterName = 'Audit Modulator'; filterEnabled = $false }
    $opacity = (Invoke-Obs 'GetSourceFilter' @{ sourceName = $inputName; filterName = 'Audit Color' }).filterSettings.opacity
    if ([Math]::Abs($opacity - 0.6) -gt 0.01) { throw "Baseline expected opacity 0.6, got $opacity" }
    $checks.Add(@{ check = 'baseline_restore'; result = 'PASS'; opacity = $opacity })
    $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = 'Audit Modulator' }
    $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = 'Audit Color' }
    # An empty media source has audio capability without playing a sound. Its name is unique to this run.
    $null = Invoke-Obs 'CreateInput' @{ sceneName = $sceneName; inputName = $audioName; inputKind = 'ffmpeg_source'; inputSettings = @{ is_local_file = $true; local_file = '' }; sceneItemEnabled = $true }; $audioCreated = $true
    Set-Shader @{ tap_mode = 7; tap_source_name = $audioName }
    Start-Sleep -Milliseconds 1200
    $null = Invoke-Obs 'RemoveInput' @{ inputName = $audioName }; $audioCreated = $false
    Start-Sleep -Milliseconds 300
    $null = Invoke-Obs 'CreateInput' @{ sceneName = $sceneName; inputName = $audioName; inputKind = 'ffmpeg_source'; inputSettings = @{ is_local_file = $true; local_file = '' }; sceneItemEnabled = $true }; $audioCreated = $true
    Start-Sleep -Milliseconds 1500
    $logFile = Get-ChildItem -LiteralPath "$ObsRoot\config\obs-studio\logs" -Filter '*.txt' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $attachments = @(Get-Content -LiteralPath $logFile.FullName | Select-String -SimpleMatch "Attached audio tap to named source '$audioName'").Count
    if ($attachments -lt 2) { throw "Named-source recovery expected two attachments, got $attachments" }
    $checks.Add(@{ check = 'named_source_recovery'; result = 'PASS'; attachments = $attachments })
    Set-Shader @{ tap_mode = 0 }
    $null = Invoke-Obs 'RemoveInput' @{ inputName = $audioName }; $audioCreated = $false
    $null = Invoke-Obs 'RemoveSourceFilter' @{ sourceName = $inputName; filterName = $filterName }
    # Every built-in compiles; repeatedly switching feedback to a nonfeedback preset exercises cache release.
    $shaders = Get-ChildItem -LiteralPath "$PSScriptRoot\..\data\shaders" -Filter '*.shader'
    foreach ($shader in $shaders) {
        $null = Invoke-Obs 'CreateSourceFilter' @{ sourceName = $inputName; filterName = $shader.Name; filterKind = 'visual_sidechain_shader_filter'; filterSettings = @{ preset_shader = $shader.Name; override_entire_effect = $true } }
    }
    for ($i = 0; $i -lt 8; $i++) {
        $null = Invoke-Obs 'SetSourceFilterSettings' @{ sourceName = $inputName; filterName = 'fft-feedback-trails.shader'; filterSettings = @{ preset_shader = 'fft-spectral-aura.shader' }; overlay = $true }
        $null = Invoke-Obs 'SetSourceFilterSettings' @{ sourceName = $inputName; filterName = 'fft-feedback-trails.shader'; filterSettings = @{ preset_shader = 'fft-feedback-trails.shader' }; overlay = $true }
    }
    $null = Get-Pixel
    $checks.Add(@{ check = 'builtin_shaders_and_feedback_churn'; result = 'PASS'; shaders = $shaders.Count })
    $samples = [System.Collections.Generic.List[object]]::new()
    for ($i = 0; $i -lt $StressSeconds; $i++) {
        $stats = Invoke-Obs 'GetStats' @{}
        $process = Get-Process -Id $portableObs.Id -ErrorAction Stop
        $samples.Add([pscustomobject]@{ second = $i; render_ms = $stats.averageFrameRenderTime; fps = $stats.activeFps; private_mb = [Math]::Round($process.PrivateMemorySize64 / 1MB, 2); handles = $process.HandleCount; rendered = $stats.renderTotalFrames; skipped = $stats.renderSkippedFrames })
        Start-Sleep -Milliseconds 1000
    }
    [pscustomobject]@{ passed_count = @($checks | Where-Object { $_.result -eq 'PASS' }).Count; expected_count = $expectedCheckCount; checks = $checks; expected_warnings = $expectedWarnings; stress_samples = $samples; restored_scene = $originalScene } | ConvertTo-Json -Depth 8
    if ($checks.Count -ne $expectedCheckCount) { throw "Expected $expectedCheckCount live checks, got $($checks.Count)" }
    if (@($checks | Where-Object { $_.result -eq 'FAIL' }).Count) { throw 'One or more live checks failed; see results above.' }
} finally {
    if ($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
        try { if ($originalScene) { $null = Invoke-Obs 'SetCurrentProgramScene' @{ sceneName = $originalScene } } } catch { Write-Warning $_ }
        foreach ($slot in $originalFaders.Keys) { try { $null = Invoke-Fadester 'set_fader_value' @{ slot = $slot; value = $originalFaders[$slot] } } catch { Write-Warning $_ } }
        try { if ($audioCreated) { $null = Invoke-Obs 'RemoveInput' @{ inputName = $audioName } } } catch { Write-Warning $_ }
        try { if ($inputCreated) { $null = Invoke-Obs 'RemoveInput' @{ inputName = $inputName } } } catch { Write-Warning $_ }
        try { if ($sceneCreated) { $null = Invoke-Obs 'RemoveScene' @{ sceneName = $sceneName } } } catch { Write-Warning $_ }
        $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, 'audit complete', $ct).GetAwaiter().GetResult() | Out-Null
    }
    $ws.Dispose()
}
