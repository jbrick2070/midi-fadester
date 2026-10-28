# SPDX-License-Identifier: GPL-2.0-or-later
# Portable OBS 33 GetSourceScreenshot pixel self-test for Visual Sidechain: Shader Filter (R-SHD-5)
# Verifies:
#   (a) A pass-through shader on a solid color source reproduces the source color within 1 %.
#   (b) A shader that outputs the source at 50 % opacity over black measures 50 % brightness (within 1 %), not 25 %.

$ErrorActionPreference = "Stop"

$existingObs = Get-Process -Name "obs64" -ErrorAction SilentlyContinue
if ($existingObs) {
    Write-Error "ABORT_OBS_ALREADY_RUNNING: obs64.exe is already running (PID=$($existingObs.Id -join ',')). Stopping per operator lock rule."
    exit 42
}

$obsRoot = "C:\Users\jeffr\OBS-Studio-33-Portable"
$obsBin  = "$obsRoot\bin\64bit"
$wsCfg   = Get-Content "$obsRoot\config\obs-studio\plugin_config\obs-websocket\config.json" | ConvertFrom-Json
$port    = [int]$wsCfg.server_port
$pw      = [string]$wsCfg.server_password

function Convert-SrgbToLinear([double]$u) {
    if ($u -le 0.04045) { return $u / 12.92 }
    return [Math]::Pow(($u + 0.055) / 1.055, 2.4)
}

$proc = Start-Process -FilePath "$obsBin\obs64.exe" -ArgumentList "--portable","--disable-updater","--multi" -WorkingDirectory $obsBin -PassThru

$allPassed = $false
try {
    $deadline = (Get-Date).AddSeconds(15)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        $tcp = New-Object System.Net.Sockets.TcpClient
        try {
            $iar = $tcp.BeginConnect("127.0.0.1", $port, $null, $null)
            if ($iar.AsyncWaitHandle.WaitOne(300) -and $tcp.Connected) {
                $ready = $true
                $tcp.Close()
                break
            }
        } catch {}
        $tcp.Close()
        Start-Sleep -Milliseconds 200
    }
    if (-not $ready) { throw "Timed out waiting for obs-websocket TCP port $port" }
    Start-Sleep -Milliseconds 600

    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    $ct = [System.Threading.CancellationToken]::None
    $ws.ConnectAsync([Uri]"ws://127.0.0.1:$port", $ct).GetAwaiter().GetResult() | Out-Null

    function Receive-WsJson {
        $ms = New-Object System.IO.MemoryStream
        $buf = New-Object byte[] 65536
        do {
            $seg = New-Object System.ArraySegment[byte]($buf, 0, $buf.Length)
            $res = $ws.ReceiveAsync($seg, $ct).GetAwaiter().GetResult()
            $ms.Write($buf, 0, $res.Count)
        } while (-not $res.EndOfMessage)
        $str = [System.Text.Encoding]::UTF8.GetString($ms.ToArray())
        return ($str | ConvertFrom-Json)
    }

    function Send-WsJson($obj) {
        $json = $obj | ConvertTo-Json -Depth 10 -Compress
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
        $seg = New-Object System.ArraySegment[byte]($bytes, 0, $bytes.Length)
        $ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).GetAwaiter().GetResult() | Out-Null
    }

    function Call-ObsReq($reqType, $reqData) {
        $reqId = [Guid]::NewGuid().ToString()
        Send-WsJson @{ op = 6; d = @{ requestType = $reqType; requestId = $reqId; requestData = $reqData } }
        while ($true) {
            $msg = Receive-WsJson
            if ($msg.op -eq 7 -and $msg.d.requestId -eq $reqId) {
                return $msg.d
            }
        }
    }

    $hello = Receive-WsJson
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $secretBytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($pw + $hello.d.authentication.salt))
    $secretB64 = [Convert]::ToBase64String($secretBytes)
    $authBytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($secretB64 + $hello.d.authentication.challenge))
    $authB64 = [Convert]::ToBase64String($authBytes)

    Send-WsJson @{ op = 1; d = @{ rpcVersion = 1; authentication = $authB64; eventSubscriptions = 0 } }
    $identified = Receive-WsJson
    if ($identified.op -ne 2) { throw "obs-websocket authentication failed" }

    function Measure-SceneScreenshot($sceneName) {
        $resp = Call-ObsReq "GetSourceScreenshot" @{
            sourceName  = $sceneName
            imageFormat = "bmp"
            imageWidth  = 64
            imageHeight = 64
        }
        if (-not $resp.requestStatus.result) {
            throw "GetSourceScreenshot failed: $($resp.requestStatus.comment)"
        }
        $b64 = $resp.responseData.imageData -replace "^data:image/[^;]+;base64,", ""
        $bytes = [Convert]::FromBase64String($b64)
        $pixelOffset = [BitConverter]::ToInt32($bytes, 10)
        $width = [BitConverter]::ToInt32($bytes, 18)
        $height = [Math]::Abs([BitConverter]::ToInt32($bytes, 22))
        $bpp = [BitConverter]::ToInt16($bytes, 28)
        $bytesPerPixel = [int]($bpp / 8)
        $rowStride = [int]([Math]::Floor(($width * $bytesPerPixel + 3) / 4) * 4)

        $sumLinR = 0.0; $sumLinG = 0.0; $sumLinB = 0.0
        $sumSrgbR = 0.0; $sumSrgbG = 0.0; $sumSrgbB = 0.0
        $count = $width * $height
        for ($y = 0; $y -lt $height; $y++) {
            $rowStart = $pixelOffset + $y * $rowStride
            for ($x = 0; $x -lt $width; $x++) {
                $px = $rowStart + $x * $bytesPerPixel
                $b = $bytes[$px] / 255.0
                $g = $bytes[$px + 1] / 255.0
                $r = $bytes[$px + 2] / 255.0
                $sumSrgbR += $r; $sumSrgbG += $g; $sumSrgbB += $b
                $sumLinR += (Convert-SrgbToLinear $r)
                $sumLinG += (Convert-SrgbToLinear $g)
                $sumLinB += (Convert-SrgbToLinear $b)
            }
        }
        return @{
            srgb_r = $sumSrgbR / $count
            srgb_g = $sumSrgbG / $count
            srgb_b = $sumSrgbB / $count
            lin_r  = $sumLinR / $count
            lin_g  = $sumLinG / $count
            lin_b  = $sumLinB / $count
        }
    }

    $origScene = (Call-ObsReq "GetCurrentProgramScene" @{}).responseData.currentProgramSceneName
    $null = Call-ObsReq "CreateScene" @{ sceneName = "__vs_selftest_scene__" }
    $null = Call-ObsReq "SetCurrentProgramScene" @{ sceneName = "__vs_selftest_scene__" }
    $null = Call-ObsReq "CreateInput" @{
        sceneName        = "__vs_selftest_scene__"
        inputName        = "__vs_selftest_color__"
        inputKind        = "color_source_v3"
        inputSettings    = @{ color = 4278190335; width = 1920; height = 1080 }
        sceneItemEnabled = $true
    }
    Start-Sleep -Milliseconds 250
    $srcColor = Measure-SceneScreenshot "__vs_selftest_scene__"

    # Check (a): Pass-through shader on solid color source must reproduce source color within 1 %
    $shaderPassThrough = "float4 mainImage(VertData v_in) : TARGET`n{`n`treturn image.Sample(texture_sampler, v_in.uv);`n}`n"
    $null = Call-ObsReq "CreateSourceFilter" @{
        sourceName     = "__vs_selftest_color__"
        filterName     = "__vs_selftest_shader__"
        filterKind     = "visual_sidechain_shader_filter"
        filterSettings = @{
            preset_shader          = "__custom_text__"
            override_entire_effect = $false
            shader_text            = $shaderPassThrough
        }
    }
    Start-Sleep -Milliseconds 300
    $resA = Measure-SceneScreenshot "__vs_selftest_scene__"
    $errA_R = [Math]::Abs($resA.lin_r - $srcColor.lin_r)
    $errA_G = [Math]::Abs($resA.lin_g - $srcColor.lin_g)
    $errA_B = [Math]::Abs($resA.lin_b - $srcColor.lin_b)
    $maxErrA = [Math]::Max($errA_R, [Math]::Max($errA_G, $errA_B))
    $passA = ($maxErrA -le 0.01)
    $statusA = if ($passA) { "PASS" } else { "FAIL" }
    Write-Host ("[visual-sidechain] SCREENSHOT CHECK (a) pass-through [{0}]: measured_lin=({1:F4}, {2:F4}, {3:F4}) source_lin=({4:F4}, {5:F4}, {6:F4}) max_err={7:F4} (<= 0.0100)" -f $statusA, $resA.lin_r, $resA.lin_g, $resA.lin_b, $srcColor.lin_r, $srcColor.lin_g, $srcColor.lin_b, $maxErrA)

    # Check (b): 50 % opacity shader over black must measure 50 % brightness (within 1 %), not 25 %
    $shaderHalfOpacity = "float4 mainImage(VertData v_in) : TARGET`n{`n`treturn image.Sample(texture_sampler, v_in.uv) * 0.5;`n}`n"
    $null = Call-ObsReq "SetSourceFilterSettings" @{
        sourceName     = "__vs_selftest_color__"
        filterName     = "__vs_selftest_shader__"
        filterSettings = @{
            preset_shader          = "__custom_text__"
            override_entire_effect = $false
            shader_text            = $shaderHalfOpacity
        }
        overlay        = $true
    }
    Start-Sleep -Milliseconds 300
    $resB = Measure-SceneScreenshot "__vs_selftest_scene__"
    $expectedB = 0.50 * $srcColor.lin_r
    $errB = [Math]::Abs($resB.lin_r - $expectedB)
    $passB = ($errB -le 0.01)
    $statusB = if ($passB) { "PASS" } else { "FAIL" }
    Write-Host ("[visual-sidechain] SCREENSHOT CHECK (b) 50% opacity  [{0}]: measured_lin=({1:F4}, {2:F4}, {3:F4}) srgb=({4:F4}, {5:F4}, {6:F4}) expected_lin=({7:F4}, 0.0000, 0.0000) err={8:F4} (<= 0.0100)" -f $statusB, $resB.lin_r, $resB.lin_g, $resB.lin_b, $resB.srgb_r, $resB.srgb_g, $resB.srgb_b, $expectedB, $errB)

    if ($origScene -and $origScene -ne "__vs_selftest_scene__") {
        $null = Call-ObsReq "SetCurrentProgramScene" @{ sceneName = $origScene }
    }
    $null = Call-ObsReq "RemoveInput" @{ inputName = "__vs_selftest_color__" }
    $null = Call-ObsReq "RemoveScene" @{ sceneName = "__vs_selftest_scene__" }
    $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "done", $ct).GetAwaiter().GetResult() | Out-Null

    $allPassed = ($passA -and $passB)
} finally {
    if ($proc -and -not $proc.HasExited) {
        Write-Host "OBS remains open (PID=$($proc.Id)). Close it manually, then inspect its shutdown leak report."
    }
}

if (-not $allPassed) {
    Write-Host "[visual-sidechain] GetSourceScreenshot self-test FAILED"
    exit 1
}
Write-Host "[visual-sidechain] GetSourceScreenshot self-test PASSED (check_a=PASS check_b=PASS)"
exit 0
