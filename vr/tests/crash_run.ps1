$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskVr = Join-Path $taskRepo 'vr'
$taskOutput = Join-Path $taskRepo 'dist\validation\crash-capture'
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskBuild = Join-Path $taskOutput 'build-tests.cmd'
@"
@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "$taskVr"
cl /nologo /EHsc /std:c++17 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"$taskOutput\\" /Fe"$taskOutput\crash_tests.exe" tests\crash_tests.cpp src\common.cpp src\crash.cpp /link dbghelp.lib user32.lib || exit /b 1
"@ | Set-Content -LiteralPath $taskBuild -Encoding ascii
& $taskBuild
if ($LASTEXITCODE -ne 0) { throw 'Crash test build failed' }

foreach ($taskCase in @('handled', 'normal', 'fatal', 'disabled', 'missing-helper')) {
    # Unique directories keep old dumps from satisfying a new run's assertions.
    $taskCasePath = Join-Path $taskOutput ("$taskCase-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $taskCasePath | Out-Null
    $taskExe = Join-Path $taskCasePath 'crash_tests.exe'
    Copy-Item -LiteralPath (Join-Path $taskOutput 'crash_tests.exe') -Destination $taskExe
    if ($taskCase -ne 'missing-helper') {
        Copy-Item -LiteralPath (Join-Path $taskVr 'bin\nfsmw_crashdump.exe') -Destination $taskCasePath
    }
    $taskMode = if ($taskCase -eq 'missing-helper') { 'fatal' } else { $taskCase }
    $taskProcess = Start-Process -FilePath $taskExe -ArgumentList $taskMode -WorkingDirectory $taskCasePath -WindowStyle Hidden -PassThru
    if (-not $taskProcess.WaitForExit(25000)) {
        Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue
        throw "$taskCase timed out"
    }
    $taskProcess.Refresh()
    $taskDump = Join-Path $taskCasePath 'nfsmw_crash.dmp'
    $taskLog = Get-Content -LiteralPath (Join-Path $taskCasePath 'nfsmw_vr.log') -Raw
    if ($taskCase -eq 'fatal') {
        if (!(Test-Path -LiteralPath $taskDump)) { throw 'Fatal exception did not produce a dump' }
        & (Join-Path $taskOutput 'crash_tests.exe') verify $taskDump
        if ($LASTEXITCODE -ne 0) { throw 'Invalid crash dump streams' }
        $taskReport = Get-Content -LiteralPath (Join-Path $taskCasePath 'nfsmw_crash.txt') -Raw
        if ($taskReport -notmatch 'Dump written: yes' -or $taskReport -notmatch 'Module:' -or $taskReport -notmatch 'Access: write') {
            throw 'Missing crash report details'
        }
    } elseif (Test-Path -LiteralPath $taskDump) { throw "$taskCase unexpectedly produced a dump" }
    if ($taskCase -eq 'disabled' -and $taskLog -notmatch 'automatic dumps disabled') { throw 'Missing disabled diagnostic' }
    if ($taskCase -eq 'missing-helper' -and $taskLog -notmatch 'cannot start') { throw 'Missing helper diagnostic' }
    if ($taskCase -in @('normal', 'handled') -and $taskProcess.ExitCode -ne 0) { throw "$taskCase failed" }
    if ($taskCase -notin @('normal', 'handled') -and $taskProcess.ExitCode -eq 0) { throw "$taskCase did not exit after its fatal exception" }
    # Helpers must exit on both normal game shutdown and a crash.
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        $taskHelperPath = Join-Path $taskCasePath 'nfsmw_crashdump.exe'
        $taskHelpers = @(Get-Process -Name nfsmw_crashdump -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $taskHelperPath })
        if (!$taskHelpers.Count) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $taskDeadline)
    if ($taskHelpers.Count) { throw "$taskCase left a helper running" }
    Write-Output "$taskCase : PASS"
}
