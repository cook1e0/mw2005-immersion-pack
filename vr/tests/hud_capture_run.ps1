param([Parameter(Mandatory=$true)][string[]]$Captures)
$ErrorActionPreference = 'Stop'
$checker = Join-Path $PSScriptRoot '..\obj\tests\hud_probe_render_tests.exe'
if (-not (Test-Path -LiteralPath $checker)) {
    throw 'Build the checker first with vr\tests\hud_probe_run.cmd.'
}
$checked = 0
$failed = 0
foreach ($pattern in $Captures) {
    foreach ($file in Get-Item -Path $pattern) {
        if ($file.Name -notmatch '_draws\.csv$') { throw "Expected a draws CSV: $($file.FullName)" }
        Write-Output $file.Name
        & $checker --replay ($file.FullName -replace '_draws\.csv$','')
        switch ($LASTEXITCODE) {
            0 { $checked++ }
            1 { $failed++ }
            2 { Write-Output 'Skipped: no dashboard component draws in this capture.' }
            default { throw "Replay crashed or failed with exit $LASTEXITCODE" }
        }
    }
}
if ($failed) { throw "$failed captures exceeded their production bounds." }
if (-not $checked) { throw 'No capture contained dashboard component draws.' }
Write-Output "$checked captures checked with no visible component overflow. Masked map quads are excluded from bounds failures."
