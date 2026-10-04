# Regression tests for C6 provenance and resume.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paged_kv_c6_provenance.ps1')
$script:failures = 0
function Check([bool]$ok, [string]$label) { if ($ok) { Write-Output "ok $label" } else { Write-Output "FAIL $label"; $script:failures++ } }
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("pkv-c6-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $base = @{ Head = ('a' * 40); Binary = 'C:\test.exe'; BinarySha256 = ('b' * 64); Inputs = [ordered]@{ artifact = ('c' * 64) } }
    $marker = Join-Path $tmp 'leg.done'
    $old = New-PkvLegRecord -Leg '7.4/C6' -Rests @('timing74') @base
    $old.Outcome = 'GREEN exit=0'
    Write-PkvLegRecord $marker $old
    $read = Read-PkvLegRecord $marker
    Check (Test-PkvLegRecordCurrent $read $old) 'record round trip'
    Check ((Get-PkvLegStanding -Rests @('timing74') -Record $read) -like 'quarantined*') 'uncommissioned 7.4 quarantined'
    $verified = Get-PkvRunnerCommissionedIds -Requested @('timing74') -VerifyLines @('QUARANTINED   PKV-TIMING-74-C6')
    Check ($verified.Count -eq 0) 'caller flag without receipt-verifier result confers no standing'
    $verified = Get-PkvRunnerCommissionedIds -VerifyLines @('COMMISSIONED  PKV-TIMING-74-C6')
    Check ($verified -contains 'timing74') 'exact verified receipt confers timing74 standing'
    $verified = Get-PkvVerifiedIds @('COMMISSIONED  PKV-LEGACY-COUNT-C4')
    Check ($verified -notcontains 'admission') 'partial grouped admission receipts confer no standing'
    $next = New-PkvLegRecord -Leg '7.4/C6' -Rests @('timing74') -Commissioned @('timing74') @base
    Check (-not (Test-PkvLegRecordCurrent $read $next)) 'commissioning invalidates marker'
    Check ((Get-PkvLegRecordDiff $read $next) -join ',' -eq 'Commissioned') 'commissioning difference named'
    Check ((Get-PkvLegStanding -Rests @('timing74') -Record $next) -eq 'standing') 'commissioned 7.4 standing without arbitrary ratio'
    Check ((Get-PkvRecordEnv $next).Count -eq 0) '7.4 has no assertion environment'
    $hooked = New-PkvLegRecord -Leg '7.4/C6' -Rests @('timing74') -Commissioned @('timing74') -HookState 'adopt=1;reset=' @base
    Check ((Get-PkvLegStanding -Rests @('timing74') -Record $hooked) -like 'quarantined*') 'mutant hook never publishes a graded 7.4 reading'
    Check (-not (Test-PkvLegRecordCurrent $hooked $next)) 'hook state changes resume identity'
    Check (Test-PkvLegRecordCurrent $next (New-PkvLegRecord -Leg '7.4/C6' -Rests @('timing74') -Commissioned @('timing74','oracle') @base)) 'unrelated commission does not invalidate'
    foreach ($key in 'Head', 'BinarySha256') {
        $changed = @{} + $base; $changed[$key] = ('d' * $base[$key].Length)
        Check (-not (Test-PkvLegRecordCurrent $next (New-PkvLegRecord -Leg '7.4/C6' -Rests @('timing74') -Commissioned @('timing74') @changed))) "$key invalidates marker"
    }
    $changed = @{} + $base; $changed.Inputs = [ordered]@{ artifact = ('d' * 64) }
    Check (-not (Test-PkvLegRecordCurrent $next (New-PkvLegRecord -Leg '7.4/C6' -Rests @('timing74') -Commissioned @('timing74') @changed))) 'input digest invalidates marker'
    $life = New-PkvLegRecord -Leg 'lifecycle/C6' -Rests @('lifecycle') -Commissioned @('lifecycle') @base
    Check ((Get-PkvRecordEnv $life)['SUPERSLM_PAGED_KV_LIFE_COMMISSIONED'] -eq '1') 'lifecycle asserts only its own variable'
    Check ((Get-PkvLegStanding -Rests @('lifecycle') -Record $life) -eq 'standing') 'lifecycle commissioned standing'
    $life.AssertEnv = ''
    Check ((Get-PkvLegStanding -Rests @('lifecycle') -Record $life) -like 'quarantined*') 'missing lifecycle assertion quarantined'
    $lifeMutant = New-PkvLegRecord -Leg 'lifecycle/C6' -Rests @('lifecycle') -Commissioned @('lifecycle') -HookState 'reset=1' @base
    Check ((Get-PkvLegStanding -Rests @('lifecycle') -Record $lifeMutant) -like 'quarantined*') 'lifecycle reset mutant cannot publish standing'
    $life['LoadBefore'] = 'load=88; offenders=UnrealEditor'
    $life['LoadAfter'] = 'load=91; offenders=cl'
    Check ($life.LoadBefore -match '88' -and $life.LoadAfter -match 'cl') 'load and offenders record even when busy'
    $lock = Join-Path $tmp 'box-global.lock'
    $held = Enter-PkvTimingLock -Path $lock
    try {
        try { Enter-PkvTimingLock -Path $lock | Out-Null; Check $false 'distinct timing leg refused by global lock' }
        catch { Check ($_.Exception.Message -match 'second C6 timing runner') 'distinct timing leg refused by global lock' }
    } finally { Exit-PkvTimingLock $held }
    Check (-not (Test-Path -LiteralPath $lock)) 'global lock released after publication'
    Set-Content -LiteralPath $lock -Value '99999999' -Encoding ascii
    $held = Enter-PkvTimingLock -Path $lock
    Check ($held -eq $lock) 'dead pid lock reclaimed'
    Exit-PkvTimingLock $held
    Set-Content -LiteralPath $marker -Value 'GREEN exit=0' -Encoding utf8
    Check ($null -eq (Read-PkvLegRecord $marker)) 'legacy marker invalid'
    try { Get-PkvCommissionedIds @('timing79') | Out-Null; Check $false 'obsolete timing79 refused' }
    catch { Check ($_.Exception.Message -match 'not .timing79.') 'obsolete timing79 refused' }
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force }
if ($script:failures) { throw "$script:failures provenance checks failed" }
