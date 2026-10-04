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
    $next = New-PkvLegRecord -Leg '7.4/C6' -Rests @('timing74') -Commissioned @('timing74') @base
    Check (-not (Test-PkvLegRecordCurrent $read $next)) 'commissioning invalidates marker'
    Check ((Get-PkvLegRecordDiff $read $next) -join ',' -eq 'Commissioned') 'commissioning difference named'
    Check ((Get-PkvLegStanding -Rests @('timing74') -Record $next) -eq 'standing') 'commissioned 7.4 standing without arbitrary ratio'
    Check ((Get-PkvRecordEnv $next).Count -eq 0) '7.4 has no assertion environment'
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
    Set-Content -LiteralPath $marker -Value 'GREEN exit=0' -Encoding utf8
    Check ($null -eq (Read-PkvLegRecord $marker)) 'legacy marker invalid'
    try { Get-PkvCommissionedIds @('timing79') | Out-Null; Check $false 'obsolete timing79 refused' }
    catch { Check ($_.Exception.Message -match 'not .timing79.') 'obsolete timing79 refused' }
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force }
if ($script:failures) { throw "$script:failures provenance checks failed" }
