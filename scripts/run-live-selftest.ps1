# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [string]$RepoDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$ObsRoot = "C:\Users\jeffr\OBS-Studio-33-Portable",
    [string]$SharedObsRoot = "" # no longer used: the run changes no plugin folder for good (H-01)
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path (Join-Path $ObsRoot "bin\64bit\obs64.exe"))) {
    throw "Test OBS folder not found at '$ObsRoot'. It holds only settings; its bin, core and data link to the installed OBS 33 (TESTING.md, section 3)."
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
# One OBS on this machine: the test folder borrows the installed OBS and shares its plugin folder, so any running
# obs64.exe (the operator's show or another run) holds the DLL this script replaces. Refuse; never close it.
if ($obsProcs.Count -gt 0) {
    throw "obs64.exe is running (PID $(($obsProcs | ForEach-Object { $_.ProcessId }) -join ', ')). Close it first; this run never closes another OBS."
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

# 4. The plugin folder. The test OBS's plugins\midi-fadester is a link into the one plugin folder the operator's own
#    OBS loads, so what is in it now is copied aside before anything is put there, and put back when the run ends,
#    pass or fail (H-01). The operator's OBS is left with exactly the files it had: never the self-test build, and
#    never some other build that happens to lie in build_x64.
$selfTestCache = Join-Path $RepoDir "build_x64\CMakeCache.txt"
if (-not (Test-Path $selfTestCache) -or
    -not (Select-String -Path $selfTestCache -Pattern '^MIDI_FADESTER_SELF_TEST:BOOL=(ON|TRUE|1)\s*$' -Quiet)) {
    throw "build_x64 was not configured with the self-test harness (H-02). Run: cmake --preset windows-x64 -DMIDI_FADESTER_SELF_TEST=ON, then cmake --build --preset windows-x64 --config RelWithDebInfo. Configure with -DMIDI_FADESTER_SELF_TEST=OFF again afterwards."
}
$destPluginDir = Join-Path $resolvedObsRoot "plugins\midi-fadester"
$pluginDirExisted = Test-Path $destPluginDir
$pluginBackupDir = Join-Path ([System.IO.Path]::GetTempPath()) ("mfd-selftest-plugin-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $pluginBackupDir | Out-Null
if ($pluginDirExisted) {
    Copy-Item (Join-Path $destPluginDir "*") $pluginBackupDir -Recurse -Force
}
$pluginDeployed = $false

# 5. Start loopMIDI only if not already running; remember ownership so we only stop what we started
$loopMidiAlreadyRunning = (@(Get-Process loopMIDI -ErrorAction SilentlyContinue).Count -gt 0)
$startedLoopProc = $null
if (-not $loopMidiAlreadyRunning) {
    $loopMidiExe = "C:\Program Files (x86)\Tobias Erichsen\loopMIDI\loopMIDI.exe"
    if (Test-Path $loopMidiExe) {
        $startedLoopProc = Start-Process -FilePath $loopMidiExe -WindowStyle Hidden -PassThru
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
    # Deploy the build under test with all of its data (shaders and the Tape Deck page too), so the run tests one
    # consistent set of files (H-01).
    $pluginDeployed = $true
    New-Item -ItemType Directory -Force -Path (Join-Path $destPluginDir "data") | Out-Null
    Copy-Item (Join-Path $RepoDir "build_x64\RelWithDebInfo\midi-fadester.dll") (Join-Path $destPluginDir "midi-fadester.dll") -Force
    Copy-Item (Join-Path $RepoDir "data\*") (Join-Path $destPluginDir "data") -Recurse -Force
    Copy-Item (Join-Path $RepoDir "build_x64\manifest.json") (Join-Path $destPluginDir "data\manifest.json") -Force

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $targetObsExe
    # OBS 33.0.1 shipped on 2026-10-29; its "New update available" dialog blocked the window close and the run was
    # killed (2026-11-01). The harness never updates OBS, so the updater stays off.
    $psi.Arguments = "--disable-updater"
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
        $_ -match 'midi-fadester|SCREENSHOT CHECK|SCENE SIGNALS|LOOPMIDI CASE|FX FILTER LIVE CHECK|SELF-TEST RESULT|Number of memory leaks'
    } | ForEach-Object { Write-Output $_ }

    if (-not $resultFound) {
        throw "SELF-TEST RESULT not found before timeout"
    }
    if ($finalLog -notmatch 'screenshot_all_pass=1') {
        throw "Self-test did not confirm screenshot_all_pass=1"
    }
    if ($finalLog -notmatch 'loopmidi_pass=1') {
        throw "Self-test did not confirm loopmidi_pass=1"
    }
    if ($finalLog -notmatch 'studio_off=1') {
        throw "Self-test did not confirm studio_off=1 before teardown"
    }
    if ($finalLog -notmatch 'fx_filter_pass=1') {
        throw "Self-test did not confirm fx_filter_pass=1"
    }
    foreach ($field in @('bridge_pass', 'build_ok', 'tbar_mid_ok', 'tbar_end_ok', 'tbar_completed')) {
        if ($finalLog -notmatch "$field=1") {
            throw "Self-test did not confirm $field=1"
        }
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

    # Put the plugin folder back as it was before the run (H-01), now that obs64 has exited.
    if ($pluginDeployed) {
        try {
            if (Test-Path $destPluginDir) {
                Get-ChildItem -LiteralPath $destPluginDir -Force | Remove-Item -Recurse -Force
            }
            if ($pluginDirExisted) {
                Copy-Item (Join-Path $pluginBackupDir "*") $destPluginDir -Recurse -Force
            }
            Remove-Item -LiteralPath $pluginBackupDir -Recurse -Force -ErrorAction SilentlyContinue
        } catch {
            Write-Warning "The plugin folder '$destPluginDir' could not be put back: $($_.Exception.Message). Its files from before the run are in '$pluginBackupDir'."
        }
    } else {
        Remove-Item -LiteralPath $pluginBackupDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
