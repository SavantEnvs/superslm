# Paged-KV plan (rev 16.2) step G6 box runner: the GPU cohort and admission cells (10.3, 10.5, 10.7) and the GPU
# decode timing (§10 R1: paged against the legacy view, bar no more than 5% slower), on each certified GPU,
# resumably. Windows PowerShell 5.1 compatible.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_paged_kv_g6_box.ps1
#            [-Scratch D:\_scratch\pkv-gpu-113] [-Model05 PATH] [-FixtureDir DIR] [-BinDir DIR] [-Adapters 0,1]
#            [-Repeats 5] [-CorrectnessOnly | -TimingOnly] [-Only 10.3/box@0,R1@1] [-Fresh] [-NoBuild]
#            [-QuietWaitSeconds 900] [-MaxLoadPercent 30] [-PauseFile D:\_scratch\TE-444\u3\PAUSE]
#
# Legs, per adapter index N in -Adapters (SSLM_GPU_ADAPTER_INDEX=N):
#   10.3/fixture@N 10.3/box@N 10.5/fixture@N 10.5/box@N 10.7/fixture@N 10.7/box@N   (correctness)
#       pkvg_suite.exe --step=G6 =<cell>/<leg>, debug layer ON (pkvg_suite sets SSLM_GPU_ENABLE_DEBUG_LAYER=1:
#       these are acceptance runs). fixture = pkv_def under -FixtureDir; box = the real 0.5B (-Model05). Both read
#       the R0 records in tests\paged-kv\reference.
#   R1@N                                                                               (timing)
#       pkvg_g6_decode_timing.exe on the real 0.5B, -Repeats repeats of 508 alternating pairs each (random order per
#       pair), debug layer OFF, under <Scratch>\timing\adapterN (its own resumable records: a rerun keeps every
#       completed repeat). The plan's R1 bar is read on the RTX 2080 SUPER; the run records which adapter it was
#       (stderr's "# adapter:" line, in the leg's .err log).
# Every correctness leg runs before any timing leg, and every build before any leg, so no compiler or GPU suite runs
# while a timing leg does.
#
# Resume: each leg writes <Scratch>\done\<leg>.done only after its process returns, holding its provenance (HEAD,
# the executables' and inputs' SHA-256, the adapter, the commissioning state) and its outcome. A leg whose marker
# matches the provenance this run would record is skipped (GREEN or RED alike, as the C6 runner does); -Fresh
# deletes the selected legs' markers (and, for timing legs, their records) first. Logs: <Scratch>\logs\<leg>.log
# and .log.err; the summary: <Scratch>\summary.txt.
#
# Timing discipline. Before the first timing leg the runner creates -PauseFile (the other box worker's pause marker),
# waits up to -QuietWaitSeconds for a quiet box, and refuses a timing leg (NOT-QUIET, nothing run) unless the box is
# quiet: no build lock (D:\_ssu_build_lock), none of the C6 runners' busy processes or another GPU suite process,
# and CPU load at most -MaxLoadPercent over three one-second samples. The harness itself refuses each repeat on the
# same process list (exit 40) and discards a repeat that ends on a busy box, so its records only ever hold quiet
# repeats. The pause marker is deleted at the end of the timing phase and on any failure or interruption (finally);
# a marker that was already present when the runner started is someone else's and is left as found.
#
# Commissioning (§8): the harness's readings are quarantined until its instrument is commissioned. The runner passes
# --commissioned only when D:\Wizard\Claude\Tooling\instrument-commission.ps1 -Verify lists
# "COMMISSIONED  PKV-TIMING-G6-R1"; otherwise the result's status is QUARANTINED (recorded, never acted on).
param(
    [string]$Scratch = 'D:\_scratch\pkv-gpu-113',
    [string]$Model05 = 'D:\hf_cache\superslm_artifacts\example\qwen2.5-0.5b-instruct-cap4096-aex.sslm',
    [string]$FixtureDir = '',
    [string]$BinDir = '',
    [int[]]$Adapters = @(0, 1),
    [int]$Repeats = 5,
    [switch]$CorrectnessOnly,
    [switch]$TimingOnly,
    [string[]]$Only = @(),
    [switch]$Fresh,
    [switch]$NoBuild,
    [int]$QuietWaitSeconds = 900,
    [int]$MaxLoadPercent = 30,
    [string]$PauseFile = 'D:\_scratch\TE-444\u3\PAUSE'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paged_kv_box_common.ps1')
if ($TimingOnly -and $CorrectnessOnly) { throw '-TimingOnly and -CorrectnessOnly exclude each other' }
$Repo = Get-PkvRepo
$RefDir = Join-Path $Repo 'tests\paged-kv\reference'
if (-not $BinDir) { $BinDir = Join-Path $Repo 'build\pkvg\bin' }
$Logs = Join-Path $Scratch 'logs'
$Done = Join-Path $Scratch 'done'
$TimingRoot = Join-Path $Scratch 'timing'
New-Item -ItemType Directory -Force -Path $Scratch, $Logs, $Done, $TimingRoot | Out-Null
$Scratch = (Resolve-Path -LiteralPath $Scratch).Path
$RegistryId = 'PKV-TIMING-G6-R1'

# ---- the legs ------------------------------------------------------------------------------------
$legs = @()
foreach ($a in $Adapters) {
    foreach ($cell in @('10.3', '10.5', '10.7')) {
        foreach ($kind in @('fixture', 'box')) {
            $legs += [pscustomobject]@{ Name = "$cell/$kind@$a"; Kind = 'correctness'; Adapter = $a; Job = "$cell/$kind"; Leg = $kind
                Exe = "g1e_cell_$($cell -replace '\.', '_').exe" }
        }
    }
}
foreach ($a in $Adapters) {
    $legs += [pscustomobject]@{ Name = "R1@$a"; Kind = 'timing'; Adapter = $a; Job = ''; Leg = 'box'; Exe = 'pkvg_g6_decode_timing.exe' }
}
if ($TimingOnly) { $legs = @($legs | Where-Object { $_.Kind -eq 'timing' }) }
if ($CorrectnessOnly) { $legs = @($legs | Where-Object { $_.Kind -eq 'correctness' }) }
if ($Only.Count -gt 0) {
    $names = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    foreach ($n in $names) { if (-not ($legs | Where-Object { $_.Name -eq $n })) { throw "-Only names no selected leg: $n" } }
    $legs = @($legs | Where-Object { $names -contains $_.Name })
}
if ($legs.Count -eq 0) { throw 'no leg selected' }
function Get-SafeName($leg) { return ($leg.Name -replace '[/\\:@]', '_') }
function Get-MarkerPath($leg) { return (Join-Path $Done ((Get-SafeName $leg) + '.done')) }
function Get-LogPath($leg) { return (Join-Path $Logs ((Get-SafeName $leg) + '.log')) }
function Get-TimingDir($leg) { return (Join-Path $TimingRoot ('adapter' + $leg.Adapter)) }
if ($Fresh) {
    foreach ($leg in $legs) {
        Remove-Item -LiteralPath (Get-MarkerPath $leg) -ErrorAction SilentlyContinue
        if ($leg.Kind -eq 'timing') { Remove-Item -LiteralPath (Get-TimingDir $leg) -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# ---- inputs ----------------------------------------------------------------------------------------
$needBox = [bool]($legs | Where-Object { $_.Leg -eq 'box' })
$needFixture = [bool]($legs | Where-Object { $_.Leg -eq 'fixture' })
if ($needBox -and -not (Test-Path -LiteralPath $Model05 -PathType Leaf)) { throw "the 0.5B artifact is not at $Model05 (-Model05)" }
if ($needFixture) {
    if (-not $FixtureDir) { $FixtureDir = $env:SUPERSLM_PAGED_KV_FIXTURE_DIR }
    if (-not $FixtureDir) { $FixtureDir = Join-Path $Scratch 'fixtures' }
    New-Item -ItemType Directory -Force -Path $FixtureDir | Out-Null
    $FixtureDir = (Resolve-Path -LiteralPath $FixtureDir).Path
    if (-not (Test-Path -LiteralPath (Join-Path $FixtureDir 'pkv_def.sslm'))) {
        Write-Output "generating the paged-KV fixtures into $FixtureDir"
        & python (Join-Path $Repo 'tools\gen_paged_kv_fixture.py') $FixtureDir | Out-Host
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $FixtureDir 'pkv_def.sslm'))) { throw 'fixture generation failed' }
    }
}

# ---- build (all before any leg) ------------------------------------------------------------------------
if (-not $NoBuild) {
    $outDir = Split-Path -Parent $BinDir
    $buildLog = Join-Path $Logs 'build.log'
    Write-Output "BUILD tests\paged-kv-gpu\build_paged_kv_gpu.bat --out=$outDir g6 (log $buildLog)"
    & cmd /c "`"$(Join-Path $Repo 'tests\paged-kv-gpu\build_paged_kv_gpu.bat')`" `"--out=$outDir`" g6 > `"$buildLog`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $buildLog | Select-Object -Last 30 | Out-Host; throw 'the G6 build failed' }
    Get-Content -LiteralPath $buildLog | Where-Object { $_ -match '^\[G6\]|PKVG BUILD|RED BY' } | Out-Host
}
$suite = Join-Path $BinDir 'pkvg_suite.exe'
foreach ($leg in $legs) {
    $e = Join-Path $BinDir $leg.Exe
    if (-not (Test-Path -LiteralPath $e -PathType Leaf)) { throw "$e is not built (build_paged_kv_gpu.bat g6)" }
}
if (($legs | Where-Object { $_.Kind -eq 'correctness' }) -and -not (Test-Path -LiteralPath $suite -PathType Leaf)) { throw "$suite is not built" }

# ---- commissioning ----------------------------------------------------------------------------------
$commissioned = $false
$verifier = 'D:\Wizard\Claude\Tooling\instrument-commission.ps1'
if (($legs | Where-Object { $_.Kind -eq 'timing' }) -and (Test-Path -LiteralPath $verifier)) {
    $verification = @(powershell -NoProfile -File $verifier -Verify 2>&1 | ForEach-Object { "$_" })
    $commissioned = [bool]($verification | Where-Object { $_ -eq "COMMISSIONED  $RegistryId" })
}

# ---- provenance and markers ------------------------------------------------------------------------------
$Head = (& git -C $Repo rev-parse HEAD)
if ($LASTEXITCODE -ne 0 -or -not $Head) { throw "git rev-parse HEAD failed in $Repo" }
$Head = "$Head".Trim()
$shaCache = @{}
function Get-CachedSha($path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return 'missing' }
    if (-not $shaCache.ContainsKey($path)) { $shaCache[$path] = Get-PkvSha256 $path }
    return $shaCache[$path]
}
function Get-LegRecord($leg) {
    $r = [ordered]@{}
    $r['Leg'] = $leg.Name
    $r['Head'] = $Head
    $r['Adapter'] = "$($leg.Adapter)"
    $r['Exe'] = Get-CachedSha (Join-Path $BinDir $leg.Exe)
    if ($leg.Kind -eq 'correctness') { $r['Suite'] = Get-CachedSha $suite }
    if ($leg.Leg -eq 'box') { $r['Model05'] = Get-CachedSha $Model05 }
    else { $r['Fixture'] = Get-CachedSha (Join-Path $FixtureDir 'pkv_def.sslm') }
    if ($leg.Kind -eq 'correctness') {
        $stem = if ($leg.Leg -eq 'box') { [IO.Path]::GetFileNameWithoutExtension($Model05) } else { 'pkv_def' }
        $r['Reference'] = Get-CachedSha (Join-Path $RefDir ('v1.11.0_' + $stem + '.ref'))
    } else {
        $r['Repeats'] = "$Repeats"
        $r['Commissioned'] = "$commissioned"
    }
    return $r
}
function Read-LegRecord($path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    $r = [ordered]@{}
    foreach ($line in (Get-Content -LiteralPath $path)) {
        $at = $line.IndexOf('=')
        if ($at -gt 0) { $r[$line.Substring(0, $at)] = $line.Substring($at + 1) }
    }
    return $r
}
function Write-LegRecord($path, $record) {
    $tmp = "$path.partial"
    ($record.Keys | ForEach-Object { "$_=$($record[$_])" }) | Set-Content -LiteralPath $tmp -Encoding ascii
    Move-Item -LiteralPath $tmp -Destination $path -Force
}
function Test-LegCurrent($stored, $record) {
    if (-not $stored) { return $false }
    foreach ($k in $record.Keys) { if ("$($stored[$k])" -ne "$($record[$k])") { return $false } }
    return $true
}

# ---- the quiet check (the C6 runners' offenders, and the GPU's own contenders) ----------------------------
function Get-QuietReport {
    $offenders = @()
    if (Test-Path -LiteralPath 'D:\_ssu_build_lock') { $offenders += 'D:\_ssu_build_lock present' }
    $procs = @(Get-Process -Name 'UnrealEditor', 'UnrealEditor-Cmd', 'cl', 'link', 'dotnet', 'UnrealBuildTool', 'ninja',
        'cmake', 'msbuild', 'cc1plus', 'smoke_adapter_driver', 'superslm_*', 'pkvg_*', 'g1d_cell_*', 'g1e_cell_*' -ErrorAction SilentlyContinue)
    foreach ($p in $procs) { $offenders += "process $($p.ProcessName) (pid $($p.Id))" }
    $load = @()
    for ($i = 0; $i -lt 3; ++$i) {
        $load += [int](Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
        Start-Sleep -Seconds 1
    }
    $avg = [int](($load | Measure-Object -Average).Average)
    if ($avg -gt $MaxLoadPercent) { $offenders += "CPU load $avg% above $MaxLoadPercent% (samples $($load -join ','))" }
    return [pscustomobject]@{ Quiet = ($offenders.Count -eq 0); Offenders = $offenders; Load = ($load -join ',') }
}

# ---- run ---------------------------------------------------------------------------------------------
$results = @{}
function Invoke-Leg($leg) {
    $marker = Get-MarkerPath $leg
    $record = Get-LegRecord $leg
    $stored = Read-LegRecord $marker
    if (Test-LegCurrent $stored $record) {
        Write-Output "SKIP $($leg.Name): done earlier with this run's provenance ($($stored['Outcome']))"
        return
    }
    if ($stored) { Write-Output "STALE $($leg.Name): its marker's provenance differs; rerunning" }
    $log = Get-LogPath $leg
    Remove-Item -Path 'Env:\SSLM_GPU_ENABLE_DEBUG_LAYER' -ErrorAction SilentlyContinue
    $env:SSLM_GPU_ADAPTER_INDEX = "$($leg.Adapter)"
    if ($leg.Kind -eq 'correctness') {
        $argv = @("--bin=$BinDir", '--step=G6', "--pkv-reference=$RefDir")
        if ($leg.Leg -eq 'box') { $argv += "--model05=$Model05" } else { $argv += "--pkv-fixtures=$FixtureDir" }
        $argv += "=$($leg.Job)"
        $exe = $suite
    } else {
        $tdir = Get-TimingDir $leg
        $argv = @("--model=$Model05", "--out=$tdir", "--repeats=$Repeats")
        if ($commissioned) { $argv += '--commissioned' }
        $exe = Join-Path $BinDir $leg.Exe
    }
    foreach ($a in $argv) { if ($a -match '\s') { throw "argument '$a' contains a space: move the path" } }
    Write-Output ("RUN  {0}: {1} {2} (log {3})" -f $leg.Name, $exe, ($argv -join ' '), $log)
    $t0 = Get-Date
    $code = Invoke-PkvLogged -Exe $exe -Arguments $argv -Log $log -WorkingDirectory $Repo
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    $tail = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue)
    if ($leg.Kind -eq 'correctness') {
        $last = $tail | Where-Object { $_ -match '^pkvg_suite at G6:' } | Select-Object -Last 1
        $verdict = switch ($code) { 0 { 'GREEN' } 1 { 'RED' } default { "CRASH($code)" } }
    } else {
        $last = $tail | Where-Object { $_ -match '^G6 R1 (pooled|verdict):' } | Select-Object -Last 2
        $last = ($last -join ' | ')
        # 0: complete, bar PASS (or a mechanism/mutant run); 1: complete with FAIL or NO RESULT, or RED on a token
        # mismatch between the arms; 40: infrastructure (refused, unbuilt, identity mismatch): resumable, no marker.
        $verdict = switch ($code) { 0 { 'COMPLETE' } 1 { 'COMPLETE' } 40 { 'INFRASTRUCTURE' } default { "CRASH($code)" } }
        if ($code -eq 1 -and ($tail | Where-Object { $_ -match '^RED:' })) { $verdict = 'RED'; $last = ($tail | Where-Object { $_ -match '^RED:' } | Select-Object -First 1) }
    }
    if ($verdict -like 'INFRASTRUCTURE*' -or $verdict -like 'CRASH*') {
        # Not a finished leg: no marker, so the next run resumes it (the harness keeps its completed repeats).
        $why = $tail | Where-Object { $_ -match '^INFRASTRUCTURE:' } | Select-Object -Last 1
        $results[$leg.Name] = "$verdict exit=$code ($secs s) $why"
        Write-Output "NOT DONE $($leg.Name): $($results[$leg.Name])"
        return
    }
    $record['Outcome'] = ('{0} exit={1} {2} ({3} s) {4}' -f $verdict, $code, $last, $secs, (Get-Date -Format s))
    Write-LegRecord $marker $record
    Write-Output "DONE $($leg.Name): $($record['Outcome'])"
}

$correctness = @($legs | Where-Object { $_.Kind -eq 'correctness' })
$timing = @($legs | Where-Object { $_.Kind -eq 'timing' })
$timingAll = $timing
foreach ($leg in $correctness) { Invoke-Leg $leg }

# Timing legs already done with this run's provenance are reported without pausing anyone.
$pending = @($timing | Where-Object { -not (Test-LegCurrent (Read-LegRecord (Get-MarkerPath $_)) (Get-LegRecord $_)) })
foreach ($leg in $timing) { if ($pending -notcontains $leg) { Invoke-Leg $leg } }
$timing = $pending
if ($timing.Count -gt 0) {
    $pauseDir = Split-Path -Parent $PauseFile
    $pauseCreated = $false
    try {
        if (Test-Path -LiteralPath $PauseFile) {
            Write-Output "PAUSE marker $PauseFile was already present: left as found"
        } else {
            New-Item -ItemType Directory -Force -Path $pauseDir | Out-Null
            Set-Content -LiteralPath $PauseFile -Value ("G6 GPU timing, pid $PID, started " + (Get-Date -Format s)) -Encoding ascii
            $pauseCreated = $true
            Write-Output "PAUSE marker created: $PauseFile"
        }
        foreach ($leg in $timing) {
            $deadline = (Get-Date).AddSeconds($QuietWaitSeconds)
            $q = Get-QuietReport
            while (-not $q.Quiet -and (Get-Date) -lt $deadline) {
                Write-Output "waiting for a quiet box before $($leg.Name): $($q.Offenders -join '; ')"
                Start-Sleep -Seconds 30
                $q = Get-QuietReport
            }
            if (-not $q.Quiet) {
                $results[$leg.Name] = "NOT-QUIET (not run): $($q.Offenders -join '; ')"
                Write-Output "REFUSED $($leg.Name): $($results[$leg.Name])"
                continue
            }
            Add-Content -LiteralPath (Join-Path $Logs 'quiet.log') -Value ("{0} {1} before: load {2}" -f (Get-Date -Format s), $leg.Name, $q.Load)
            Invoke-Leg $leg
            $q = Get-QuietReport
            Add-Content -LiteralPath (Join-Path $Logs 'quiet.log') -Value ("{0} {1} after: load {2}; {3}" -f (Get-Date -Format s), $leg.Name, $q.Load, ($q.Offenders -join '; '))
        }
    } finally {
        if ($pauseCreated) {
            Remove-Item -LiteralPath $PauseFile -Force -ErrorAction SilentlyContinue
            Write-Output "PAUSE marker removed: $PauseFile"
        }
    }
}
Remove-Item -Path 'Env:\SSLM_GPU_ADAPTER_INDEX' -ErrorAction SilentlyContinue

# ---- summary ---------------------------------------------------------------------------------------------
$rows = @()
$allDone = $true
foreach ($leg in $legs) {
    if ($results.ContainsKey($leg.Name)) { $state = $results[$leg.Name] }
    else {
        $stored = Read-LegRecord (Get-MarkerPath $leg)
        $state = if ($stored) { "$($stored['Outcome'])" } else { 'NOT-RUN' }
    }
    $verdict = ($state -split ' ')[0]
    if ($verdict -ne 'GREEN' -and $verdict -ne 'COMPLETE') { $allDone = $false }
    $rows += [pscustomobject]@{ Leg = $leg.Name; Kind = $leg.Kind; Verdict = $verdict; Detail = $state }
}
$summary = @('G6 box run summary ' + (Get-Date -Format s) + " (repo $Repo at $Head, bin $BinDir, scratch $Scratch)")
$summary += ("timing instrument {0}: {1}" -f $RegistryId, $(if ($commissioned) { 'COMMISSIONED (readings stand)' } else { 'not commissioned: readings QUARANTINED' }))
$summary += ($rows | Format-Table -AutoSize -Wrap Leg, Kind, Verdict, Detail | Out-String -Width 240).TrimEnd()
$summary += ''
$summary += 'R1 readings (pooled and per repeat, from each adapter''s result.txt):'
foreach ($leg in $timingAll) {
    $res = Join-Path (Get-TimingDir $leg) 'result.txt'
    if (Test-Path -LiteralPath $res) { Get-Content -LiteralPath $res | Where-Object { $_ -match '^G6 R1 |^adapter_index=' } | ForEach-Object { $summary += "  [$($leg.Name)] $_" } }
    $err = (Get-LogPath $leg) + '.err'
    if (Test-Path -LiteralPath $err) { Get-Content -LiteralPath $err | Where-Object { $_ -match '# adapter' } | Select-Object -First 1 | ForEach-Object { $summary += "  [$($leg.Name)] $_" } }
}
$summary | Set-Content -LiteralPath (Join-Path $Scratch 'summary.txt') -Encoding utf8
$summary | Write-Output
if (-not $allDone) { exit 1 }
