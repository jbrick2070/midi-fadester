param(
    [string]$RepoDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$ObsRoot = "C:\Users\jeffr\OBS-Studio-33-Portable-MidiFadester",
    [string]$SharedObsRoot = ""
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path (Join-Path $ObsRoot "bin\64bit\obs64.exe"))) {
    throw "Dedicated Portable OBS root not found at '$ObsRoot'. Clone OBS-Studio-33-Portable to '$ObsRoot' first."
}

$resolvedObsRoot = (Resolve-Path $ObsRoot).Path.TrimEnd('\')
$targetObsExe = (Resolve-Path (Join-Path $resolvedObsRoot "bin\64bit\obs64.exe")).Path

# 1. Never close or disturb another project's running OBS or test harness.
$obsProcs = @(Get-CimInstance Win32_Process -Filter "Name='obs64.exe'" -ErrorAction SilentlyContinue)
foreach ($p in $obsProcs) {
    if ($p.ExecutablePath -and ($p.ExecutablePath -ieq $targetObsExe)) {
        throw "obs64.exe (PID $($p.ProcessId)) is already running from '$resolvedObsRoot'. Refusing to close an active session or test run."
    }
}

$allProcs = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
$ancestorPids = New-Object 'System.Collections.Generic.HashSet[int]'
$walkPid = $PID
while ($walkPid -gt 0) {
    if (-not $ancestorPids.Add([int]$walkPid)) {
        break
    }
    $me = $allProcs | Where-Object { $_.ProcessId -eq $walkPid } | Select-Object -First 1
    if (-not $me) {
        break
    }
    $walkPid = [int]$me.ParentProcessId
}

$escapedRoot = [regex]::Escape($resolvedObsRoot)
$siblingRunners = @($allProcs | Where-Object {
    (-not $ancestorPids.Contains([int]$_.ProcessId)) -and $_.CommandLine -and (
        $_.CommandLine -match $escapedRoot -or
        ($resolvedObsRoot -ieq "C:\Users\jeffr\OBS-Studio-33-Portable" -and $_.CommandLine -match 'playlister|sidechain')
    )
})
if ($siblingRunners.Count -gt 0) {
    $summary = ($siblingRunners | ForEach-Object { "$($_.Name)($($_.ProcessId))" }) -join ", "
    throw "Another test process is referencing '$resolvedObsRoot': $summary. Aborting."
}

# 2. Back up mutable OBS config files (user.ini, Untitled.json, Untitled.json.bak, global.json) as exact bytes.
$configFilesToProtect = @(
    (Join-Path $resolvedObsRoot "config\obs-studio\user.ini"),
    (Join-Path $resolvedObsRoot "config\obs-studio\basic\scenes\Untitled.json"),
    (Join-Path $resolvedObsRoot "config\obs-studio\basic\scenes\Untitled.json.bak"),
    (Join-Path $resolvedObsRoot "config\obs-studio\plugin_config\midi-fadester\global.json")
)
$backups = @{}
foreach ($cfgPath in $configFilesToProtect) {
    if (Test-Path $cfgPath) {
        $backups[$cfgPath] = @{
            Existed = $true
            Bytes   = [System.IO.File]::ReadAllBytes($cfgPath)
        }
    } else {
        $backups[$cfgPath] = @{
            Existed = $false
            Bytes   = $null
        }
    }
}

# 3. Pre-warm midi-send.exe
$midiSendExe = Join-Path $RepoDir "build_x64\tests\RelWithDebInfo\midi-send.exe"
& $midiSendExe --self-test | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "midi-send --self-test failed"
}

# 4. Deploy RelWithDebInfo plugin (with MFD_ENABLE_SELF_TEST) + manifest + data to dedicated $ObsRoot
$destPluginDir = Join-Path $resolvedObsRoot "plugins\midi-fadester"
New-Item -ItemType Directory -Force -Path (Join-Path $destPluginDir "data\effects") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $destPluginDir "data\locale") | Out-Null
Copy-Item (Join-Path $RepoDir "build_x64\RelWithDebInfo\midi-fadester.dll") (Join-Path $destPluginDir "midi-fadester.dll") -Force
Copy-Item (Join-Path $RepoDir "build_x64\manifest.json") (Join-Path $destPluginDir "data\manifest.json") -Force
Copy-Item (Join-Path $RepoDir "data\effects\mfd_layer_mixer.effect") (Join-Path $destPluginDir "data\effects\mfd_layer_mixer.effect") -Force
Copy-Item (Join-Path $RepoDir "data\locale\en-US.ini") (Join-Path $destPluginDir "data\locale\en-US.ini") -Force

# 5. Start loopMIDI only if not already running; remember ownership so we only stop what we started
$loopMidiAlreadyRunning = (@(Get-Process loopMIDI -ErrorAction SilentlyContinue).Count -gt 0)
$startedLoopProc = $null
if (-not $loopMidiAlreadyRunning) {
    $loopMidiExe = "C:\Program Files (x86)\Tobias Erichsen\loopMIDI\loopMIDI.exe"
    if (Test-Path $loopMidiExe) {
        $startedLoopProc = Start-Process -FilePath $loopMidiExe -PassThru
        [void]$startedLoopProc.WaitForInputIdle(5000)
    }
}

$logsDir = Join-Path $resolvedObsRoot "config\obs-studio\logs"
New-Item -ItemType Directory -Force -Path $logsDir | Out-Null
$beforeTime = (Get-Date).AddSeconds(-2)

$watcher = New-Object System.IO.FileSystemWatcher($logsDir, "*.txt")
$watcher.NotifyFilter = [System.IO.NotifyFilters]::LastWrite -bor [System.IO.NotifyFilters]::FileName -bor [System.IO.NotifyFilters]::Size
$watcher.EnableRaisingEvents = $true

$obsProc = $null
try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $targetObsExe
    $psi.WorkingDirectory = (Join-Path $resolvedObsRoot "bin\64bit")
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables["MFD_SELF_TEST"] = "1"
    $psi.EnvironmentVariables["MFD_MIDI_SEND_EXE"] = ($midiSendExe -replace '\\', '/')
    $obsProc = [System.Diagnostics.Process]::Start($psi)

    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    $resultFound = $false
    $latestLog = $null

    while ([DateTime]::UtcNow -lt $deadline -and -not $obsProc.HasExited) {
        [void]$watcher.WaitForChanged([System.IO.WatcherChangeTypes]::All, 1000)
        $cand = Get-ChildItem $logsDir -Filter "*.txt" |
            Where-Object { $_.LastWriteTime -ge $beforeTime } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($cand) {
            $latestLog = $cand.FullName
            $fs = New-Object System.IO.FileStream($latestLog, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            $sr = New-Object System.IO.StreamReader($fs)
            $content = $sr.ReadToEnd()
            $sr.Close()
            $fs.Close()
            if ($content.Contains("SELF-TEST RESULT:")) {
                $resultFound = $true
                break
            }
        }
    }

    if ($obsProc -and -not $obsProc.HasExited) {
        [void]$obsProc.CloseMainWindow()
        if (-not $obsProc.WaitForExit(15000)) {
            $obsProc.Kill()
            [void]$obsProc.WaitForExit(5000)
        }
    }

    if (-not $latestLog) {
        throw "No OBS log file created in $logsDir"
    }

    $finalLog = [System.IO.File]::ReadAllText($latestLog)
    Write-Output "=== OBS LOG: $latestLog ==="
    $finalLog -split "`r?`n" | Where-Object {
        $_ -match 'midi-fadester|SCREENSHOT CHECK|SCENE SIGNALS|LOOPMIDI CASE|SELF-TEST RESULT|Number of memory leaks'
    } | ForEach-Object { Write-Output $_ }

    if (-not $resultFound) {
        throw "SELF-TEST RESULT not found before timeout"
    }
    if ($finalLog -notmatch 'studio_off=1') {
        throw "Self-test did not confirm studio_off=1 before teardown"
    }
    if ($finalLog -notmatch 'Number of memory leaks: 0') {
        throw "OBS reported non-zero memory leaks"
    }
} finally {
    $watcher.Dispose()
    if ($obsProc -and -not $obsProc.HasExited) {
        try {
            $obsProc.Kill()
            [void]$obsProc.WaitForExit(5000)
        } catch {}
    }

    if ($startedLoopProc -and -not $startedLoopProc.HasExited) {
        Stop-Process -Id $startedLoopProc.Id -Force -ErrorAction SilentlyContinue
    }

    # Restore exact pre-test user.ini, Untitled.json, Untitled.json.bak, and global.json AFTER obs64 exits
    foreach ($cfgPath in $configFilesToProtect) {
        $entry = $backups[$cfgPath]
        if ($entry.Existed) {
            $parentDir = Split-Path -Parent $cfgPath
            if (-not (Test-Path $parentDir)) {
                New-Item -ItemType Directory -Force -Path $parentDir | Out-Null
            }
            [System.IO.File]::WriteAllBytes($cfgPath, $entry.Bytes)
        } elseif (Test-Path $cfgPath) {
            Remove-Item $cfgPath -Force -ErrorAction SilentlyContinue
        }
    }

    # Deploy Release DLL to dedicated $ObsRoot
    $releaseDll = Join-Path $RepoDir "build_x64\Release\midi-fadester.dll"
    if (Test-Path $releaseDll) {
        Copy-Item $releaseDll (Join-Path $destPluginDir "midi-fadester.dll") -Force
    }

    # Optionally update shared OBS plugin binary ONLY when no obs64 or sibling harness is active there
    if (-not [string]::IsNullOrWhiteSpace($SharedObsRoot) -and (Test-Path $SharedObsRoot) -and (Test-Path $releaseDll)) {
        $sharedResolved = (Resolve-Path $SharedObsRoot).Path.TrimEnd('\')
        $sharedExe = Join-Path $sharedResolved "bin\64bit\obs64.exe"
        $sharedBusy = @(Get-CimInstance Win32_Process -Filter "Name='obs64.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.ExecutablePath -and ($_.ExecutablePath -ieq $sharedExe) }).Count -gt 0
        if (-not $sharedBusy) {
            $sharedPluginDir = Join-Path $sharedResolved "plugins\midi-fadester"
            if (Test-Path $sharedPluginDir) {
                Copy-Item $releaseDll (Join-Path $sharedPluginDir "midi-fadester.dll") -Force
                Copy-Item (Join-Path $RepoDir "build_x64\manifest.json") (Join-Path $sharedPluginDir "data\manifest.json") -Force
            }
        }
    }
}
