# Paged-KV plan (rev 16.7) step C6: the box run's per-leg provenance records and the standing derived
# from them (tools/run_paged_kv_c6_box.ps1). The functions can be tested without a timing run.
# Dot-source this file.
#
# A leg's done-marker is its provenance record: the git HEAD, the sha256 of the test binary, the
# sha256 of every file the leg read (artifacts, references, baselines), the commissioned instrument
# ids in force that the leg rests on, the assertion environment variables actually set, and
# the outcome. Recording only what
# bears on the leg means commissioning one instrument reruns only the legs that rest on it.
# A leg is skipped on resume only when its stored record equals the record this invocation would
# produce; its standing is derived from its stored record alone, never from the current arguments.

# The instruments the verified receipt may name, by the registry entry each stands for, and the environment
# variable through which the cell asserts that instrument's verdict (empty: no variable; the oracle
# and the admission instruments assert unconditionally). Lifecycle has no performance bar;
# commissioning makes its measured change report eligible to stand, while its NO RESULT/RESOLVED
# verdicts continue to assert nothing.
$PkvC6Instruments = [ordered]@{
    oracle    = @{ Registry = 'PKV-ORACLE-R0'; Env = '' }
    admission = @{ Registry = 'PKV-LEGACY-COUNT-C4, PKV-FILL-PROBE-C5, PKV-FILL-PROBE-ONESTATE-C5'; Env = '' }
    timing74  = @{ Registry = 'PKV-TIMING-74-C6-R168'; Env = '' }
    lifecycle = @{ Registry = 'PKV-LIFECYCLE-C6-R168'; Env = 'SUPERSLM_PAGED_KV_LIFE_COMMISSIONED' }
}
$PkvC6RecordSchema = 'pkv-c6-leg/4'
# The fields a stored record must match for a leg to be skipped. Outcome and the binary's path are
# kept for the reader, not compared.
$PkvC6RecordKeys = @('Schema', 'Leg', 'Head', 'BinarySha256', 'SourceManifestSha256', 'Inputs', 'Commissioned', 'AssertEnv', 'HookState')
$PkvC6TimingLock = 'D:\_scratch\SuperSLM\pkv-c6-timing.lock'

function Enter-PkvTimingLock {
    param([string]$Path = $PkvC6TimingLock)
    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    for ($attempt = 0; $attempt -lt 3; ++$attempt) {
        try {
            $file = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                $bytes = [Text.Encoding]::ASCII.GetBytes("$PID")
                $file.Write($bytes, 0, $bytes.Length)
            } finally { $file.Dispose() }
            return $Path
        } catch [IO.IOException] {
            if (-not (Test-Path -LiteralPath $Path)) { continue }
            $owner = 0
            if (-not [int]::TryParse((Get-Content -LiteralPath $Path -Raw).Trim(), [ref]$owner)) { throw "torn C6 timing lock: $Path" }
            if (Get-Process -Id $owner -ErrorAction SilentlyContinue) { throw "second C6 timing runner refused: pid $owner" }
            Remove-Item -LiteralPath $Path -Force
        }
    }
    throw "cannot acquire C6 timing lock: $Path"
}

function Exit-PkvTimingLock {
    param([string]$Path)
    if ((Test-Path -LiteralPath $Path) -and (Get-Content -LiteralPath $Path -Raw).Trim() -eq "$PID") {
        Remove-Item -LiteralPath $Path -Force
    }
}

# Splits comma-joined lists and rejects any id that is not a commissionable instrument.
function Get-PkvCommissionedIds {
    param([string[]]$Ids)
    $out = @($Ids | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    foreach ($c in $out) {
        if (-not $PkvC6Instruments.Contains($c)) {
            throw "-Commissioned takes $(@($PkvC6Instruments.Keys) -join ', '); not '$c'"
        }
    }
    return , @($out | Sort-Object -Unique)
}

# Only the receipt-verifying tool's computed status can confer standing.
# -Status prints stored fields; -Verify rechecks the receipt and live hash.
function Get-PkvVerifiedIds {
    param([string[]]$Lines)
    $found = @()
    foreach ($key in $PkvC6Instruments.Keys) {
        $registryIds = @($PkvC6Instruments[$key].Registry -split ',' | ForEach-Object { $_.Trim() })
        $verified = @($registryIds | Where-Object { $id = $_; @($Lines | Where-Object { $_ -ceq "COMMISSIONED  $id" }).Count })
        if ($verified.Count -eq $registryIds.Count) { $found += $key }
    }
    return ,$found
}

function Get-PkvRunnerCommissionedIds {
    param([string[]]$Requested = @(), [string[]]$VerifyLines = @())
    # Requested is accepted for command-line compatibility only. A verified
    # receipt line is the sole path to standing.
    return (Get-PkvVerifiedIds $VerifyLines)
}

function Get-PkvNormalizedSha256 {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $normalized = New-Object System.Collections.Generic.List[byte] ($bytes.Length)
    for ($i = 0; $i -lt $bytes.Length; ++$i) {
        if ($bytes[$i] -eq 13 -and $i + 1 -lt $bytes.Length -and $bytes[$i + 1] -eq 10) { continue }
        $normalized.Add($bytes[$i])
    }
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($normalized.ToArray())).Replace('-', '').ToLowerInvariant() }
    finally { $hash.Dispose() }
}

# The receipt verifier establishes the registry entry; this check establishes
# that its source is in this link's source set and the invoked bytes still match.
function Test-PkvSourceBinding {
    param([string]$Binary, [string]$RegistryId, [string]$Repo,
          [string]$RegistryPath = 'D:\Wizard\Claude\Tooling\instrument-registry.json')
    $checker = Join-Path $Repo 'tools/verify_paged_kv_binding.py'
    if (-not (Test-Path -LiteralPath $checker) -or -not (Test-Path -LiteralPath $RegistryPath)) { return $false }
    try {
        $result = & python $checker --binary $Binary --repo $Repo --registry $RegistryPath --id $RegistryId 2>$null
        if ($LASTEXITCODE -ne 0) { return $false }
        return [bool](($result | ConvertFrom-Json).bound)
    } catch { return $false }
}

# The assertion variables a leg resting on $Rests gets: only for an instrument that is both rested on
# and commissioned.
function Get-PkvLegAssertionEnv {
    param([string[]]$Rests, [string[]]$Commissioned)
    $envs = [ordered]@{}
    foreach ($r in @($Rests | Sort-Object -Unique)) {
        if ($Commissioned -notcontains $r -or -not $PkvC6Instruments.Contains($r)) { continue }
        switch ($r) {
            'lifecycle' { $envs['SUPERSLM_PAGED_KV_LIFE_COMMISSIONED'] = '1' }
        }
    }
    return $envs
}

function ConvertTo-PkvPairs {
    param($Map)
    if (-not $Map) { return '' }
    return (@($Map.Keys | Sort-Object | ForEach-Object { "$_=$($Map[$_])" }) -join ';')
}

# The record this invocation would write for a leg. $Inputs maps a label (role:file name) to the
# file's sha256. Commissioned keeps only the ids the leg rests on. Every value is a plain string, so the record survives a JSON round trip
# unchanged.
function New-PkvLegRecord {
    param(
        [Parameter(Mandatory = $true)][string]$Leg,
        [string[]]$Rests = @(),
        [string]$Head = '',
        [string]$Binary = '',
        [string]$BinarySha256 = '',
        [string]$SourceManifestSha256 = '',
        [System.Collections.IDictionary]$Inputs = @{},
        [string[]]$Commissioned = @(),
        [string]$HookState = ''
    )
    $envs = Get-PkvLegAssertionEnv -Rests $Rests -Commissioned $Commissioned
    $relevant = @($Commissioned | Where-Object { $Rests -contains $_ } | Sort-Object -Unique)
    return [ordered]@{
        Schema       = $PkvC6RecordSchema
        Leg          = $Leg
        Head         = $Head
        Binary       = $Binary
        BinarySha256 = $BinarySha256
        SourceManifestSha256 = $SourceManifestSha256
        Inputs       = (ConvertTo-PkvPairs $Inputs)
        Commissioned = ($relevant -join ',')
        AssertEnv    = (ConvertTo-PkvPairs $envs)
        HookState    = $HookState
        Outcome      = ''
    }
}

# The assertion variables a record says were set, as a hashtable (what the runner puts in the env).
function Get-PkvRecordEnv {
    param($Record)
    $envs = [ordered]@{}
    if (-not $Record) { return $envs }
    foreach ($p in @("$($Record.AssertEnv)" -split ';' | Where-Object { $_ })) {
        $k, $v = $p -split '=', 2
        $envs[$k] = $v
    }
    return $envs
}

function Get-PkvRecordField {
    param($Record, [string]$Key)
    if ($Record -is [System.Collections.IDictionary]) { return "$($Record[$Key])" }
    $p = $Record.PSObject.Properties[$Key]
    if ($p) { return "$($p.Value)" }
    return ''
}

# True only when both records exist, are of this schema, and agree on every compared field.
function Test-PkvLegRecordCurrent {
    param($Stored, $Current)
    if (-not $Stored -or -not $Current) { return $false }
    foreach ($k in $PkvC6RecordKeys) {
        if ((Get-PkvRecordField $Stored $k) -cne (Get-PkvRecordField $Current $k)) { return $false }
    }
    return ((Get-PkvRecordField $Stored 'Schema') -eq $PkvC6RecordSchema)
}

# The compared fields on which two records differ (for the runner's STALE line).
function Get-PkvLegRecordDiff {
    param($Stored, $Current)
    return , @($PkvC6RecordKeys | Where-Object { (Get-PkvRecordField $Stored $_) -cne (Get-PkvRecordField $Current $_) })
}

# A leg's standing, from its stored record alone. Standing needs every instrument the leg rests on to
# be commissioned in that record and, for an instrument asserted through a variable, that variable to
# have been set for that run. No record (a marker from before this schema, or none): quarantined.
function Get-PkvLegStanding {
    param([string[]]$Rests, $Record)
    if (-not $Record -or (Get-PkvRecordField $Record 'Schema') -ne $PkvC6RecordSchema) { return 'quarantined (no provenance record)' }
    if (Get-PkvRecordField $Record 'HookState') { return 'quarantined (mutant construction hook)' }
    $commissioned = @((Get-PkvRecordField $Record 'Commissioned') -split ',' | Where-Object { $_ })
    $envs = Get-PkvRecordEnv $Record
    $missing = @()
    foreach ($r in $Rests) {
        if (-not $PkvC6Instruments.Contains($r)) { $missing += "$r (never commissioned)"; continue }
        if ($commissioned -notcontains $r) { $missing += $r; continue }
        $var = $PkvC6Instruments[$r].Env
        if ($var -and -not $envs.Contains($var)) {
            $missing += "$r ($var unset)"
        }
    }
    if ($missing.Count -eq 0) { return 'standing' }
    return 'quarantined (' + ($missing -join ', ') + ')'
}

# Reads a marker; $null when it is absent or not a record of this schema (a pre-provenance marker
# holds only "GREEN exit=0 ...", which can never match, so its leg reruns).
function Read-PkvLegRecord {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try { $r = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json } catch { return $null }
    if (-not $r -or (Get-PkvRecordField $r 'Schema') -ne $PkvC6RecordSchema) { return $null }
    return $r
}

function Write-PkvLegRecord {
    param([string]$Path, $Record)
    # -InputObject: a dictionary serializes as one object on Windows PowerShell 5.1 and on pwsh; every
    # value is a string, so the default depth is never reached.
    Set-Content -LiteralPath $Path -Encoding utf8 -Value (ConvertTo-Json -InputObject $Record -Depth 3)
}
