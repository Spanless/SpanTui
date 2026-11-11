[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\toolchain.ps1')

function Assert-Throws([scriptblock]$Action, [string]$Message) {
    $threw = $false
    try { & $Action | Out-Null } catch { $threw = $true }
    if (-not $threw) { throw $Message }
}

# An empty or blank discovery set is a valid result, not a binding failure.
if ($null -ne (Get-FirstExistingPath -Candidates $null)) { throw 'An empty candidate set must resolve to null.' }
if ($null -ne (Get-FirstExistingPath -Candidates @($null, '', '   '))) { throw 'Blank candidates must resolve to null.' }

$testRoot = Join-Path (Get-SpanTuiDevPath -Name 'tmp') ("toolchain-self-test-{0}" -f [guid]::NewGuid())
try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

    # A build tree is reused only while build.ninja, a Ninja cache and the
    # recorded configure arguments all match.
    $arguments = @('-S', 'src', '-B', 'build', '-DQt6_DIR=C:\Qt\6.12.0\mingw_64\lib\cmake\Qt6')
    if (Test-SpanTuiBuildTreeCurrent -BuildDir $testRoot -Arguments $arguments) { throw 'An empty directory is not a configured tree.' }
    Set-Content -LiteralPath (Join-Path $testRoot 'build.ninja') -Value '# test'
    Set-Content -LiteralPath (Join-Path $testRoot 'CMakeCache.txt') -Value 'CMAKE_GENERATOR:INTERNAL=Ninja'
    [System.IO.File]::WriteAllText((Join-Path $testRoot '.spantui-configure'), ($arguments -join "`n"))
    if (-not (Test-SpanTuiBuildTreeCurrent -BuildDir $testRoot -Arguments $arguments)) { throw 'A matching tree must stay incremental.' }
    if (Test-SpanTuiBuildTreeCurrent -BuildDir $testRoot -Arguments ($arguments + '-DSPANTUI_INSTALLED_IMAGE=ON')) {
        throw 'Changed configure arguments must require configure.'
    }
    # A stale build.ninja must not hide a tree another tool reconfigured with
    # a different generator (e.g. MinGW Makefiles).
    Set-Content -LiteralPath (Join-Path $testRoot 'CMakeCache.txt') -Value 'CMAKE_GENERATOR:INTERNAL=MinGW Makefiles'
    if (Test-SpanTuiBuildTreeCurrent -BuildDir $testRoot -Arguments $arguments) { throw 'A non-Ninja cache must require configure.' }

    # A cache whose Qt6Core resolved under another kit must be discarded.
    Set-Content -LiteralPath (Join-Path $testRoot 'CMakeCache.txt') -Value @(
        'CMAKE_GENERATOR:INTERNAL=Ninja', 'Qt6Core_DIR:PATH=C:/Qt/6.9.3/mingw_64/lib/cmake/Qt6Core')
    if (-not (Test-SpanTuiCacheForeignQt -BuildDir $testRoot -QtRoot 'C:\Qt\6.12.0\mingw_64')) { throw 'Another kit in the cache must be detected.' }
    if (Test-SpanTuiCacheForeignQt -BuildDir $testRoot -QtRoot 'c:\qt\6.9.3\mingw_64\') { throw 'The same kit must not be foreign.' }
    if (-not (Test-SpanTuiCacheForeignQt -BuildDir $testRoot -QtRoot 'C:\Qt\6.9')) { throw 'A path prefix is not the same kit.' }

    # Output locations: target\ of the checkout by default; SPANTUI_BUILD_ROOT
    # and SPANTUI_ARTIFACT_ROOT are absolute or relative to the main checkout.
    $repoRoot = Join-Path $testRoot 'repo'
    $cases = @(
        @{ Actual = (Resolve-SpanTuiBuildDirectory -Root $repoRoot -BuildRoot '' -RetiredBuildDir ''); Expected = (Join-Path $repoRoot 'target\build') }
        @{ Actual = (Resolve-SpanTuiBuildDirectory -Root $repoRoot -BuildRoot 'D:\cache\span-tui\qt\'); Expected = 'D:\cache\span-tui\qt\main' }
        @{ Actual = (Resolve-SpanTuiBuildDirectory -Root $repoRoot -BuildRoot 'out\b'); Expected = (Join-Path $repoRoot 'out\b\main') }
        @{ Actual = (Resolve-SpanTuiReleaseDirectory -Root $repoRoot -Version '0.1.0' -ArtifactRoot ''); Expected = (Join-Path $repoRoot 'target\release\0.1.0\windows-x64') }
        @{ Actual = (Resolve-SpanTuiReleaseDirectory -Root $repoRoot -Version '0.1.0' -ArtifactRoot 'D:\artifacts'); Expected = 'D:\artifacts\span-tui\0.1.0\windows-x64' }
        @{ Actual = (Resolve-SpanTuiReleaseDirectory -Root $repoRoot -Version '0.1.0' -ArtifactRoot 'art'); Expected = (Join-Path $repoRoot 'art\span-tui\0.1.0\windows-x64') }
        @{ Actual = (Resolve-SpanTuiWorktreeRoot -Root $repoRoot -WorktreeRoot ''); Expected = "$repoRoot.worktrees" }
    )
    foreach ($case in $cases) {
        if ($case.Actual -ne $case.Expected) { throw "Output location: expected $($case.Expected), got $($case.Actual)" }
    }
    if (Test-Path -LiteralPath $repoRoot) { throw 'Resolving output locations must not create anything.' }
    # The retired single-tree variable must not silently fall back to target\.
    Assert-Throws { Resolve-SpanTuiBuildDirectory -Root $repoRoot -BuildRoot '' -RetiredBuildDir 'D:\cache\build' } `
        'SPANTUI_BUILD_DIR without SPANTUI_BUILD_ROOT must fail.'

    # Linked worktrees: a .git file names <main>\.git\worktrees\<id>, whose
    # commondir leads back. Build trees split per checkout, relative roots and
    # the default .env fall back to the main checkout.
    $mainRoot = Join-Path $testRoot 'main'
    $adminDir = Join-Path $mainRoot '.git\worktrees\feat'
    $worktreeRoot = Join-Path $testRoot 'elsewhere\feat'
    New-Item -ItemType Directory -Path $adminDir, $worktreeRoot -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $adminDir 'commondir') -Value '../..'
    Set-Content -LiteralPath (Join-Path $worktreeRoot '.git') -Value ('gitdir: ' + $adminDir.Replace('\', '/'))
    $main = Get-SpanTuiCheckout -Root $mainRoot
    $linked = Get-SpanTuiCheckout -Root $worktreeRoot
    if ($main.IsWorktree -or $main.Name -ne 'main') { throw 'A .git directory is the main checkout.' }
    if (-not $linked.IsWorktree -or $linked.Name -ne 'wt-feat' -or $linked.MainRoot -ne $mainRoot) {
        throw "Linked worktree misread: $($linked | Out-String)"
    }
    $cases = @(
        @{ Actual = (Resolve-SpanTuiBuildDirectory -Root $worktreeRoot -BuildRoot '' -RetiredBuildDir ''); Expected = (Join-Path $worktreeRoot 'target\build') }
        @{ Actual = (Resolve-SpanTuiBuildDirectory -Root $worktreeRoot -BuildRoot 'D:\cache\span-tui\qt'); Expected = 'D:\cache\span-tui\qt\wt-feat' }
        @{ Actual = (Resolve-SpanTuiBuildDirectory -Root $worktreeRoot -BuildRoot 'out'); Expected = (Join-Path $mainRoot 'out\wt-feat') }
        @{ Actual = (Resolve-SpanTuiReleaseDirectory -Root $worktreeRoot -Version '0.1.0' -ArtifactRoot ''); Expected = (Join-Path $worktreeRoot 'target\release\0.1.0\windows-x64') }
        @{ Actual = (Resolve-SpanTuiWorktreeRoot -Root $worktreeRoot -WorktreeRoot ''); Expected = "$mainRoot.worktrees" }
        @{ Actual = (Resolve-SpanTuiWorktreeRoot -Root $worktreeRoot -WorktreeRoot 'wt'); Expected = (Join-Path $mainRoot 'wt') }
    )
    foreach ($case in $cases) {
        if ($case.Actual -ne $case.Expected) { throw "Worktree location: expected $($case.Expected), got $($case.Actual)" }
    }
    Set-Content -LiteralPath (Join-Path $mainRoot '.env') -Value @('SPANTUI_SELFTEST_SHARED=main', 'SPANTUI_SELFTEST_OWN=main')
    Set-Content -LiteralPath (Join-Path $worktreeRoot '.env') -Value 'SPANTUI_SELFTEST_OWN=worktree'
    try {
        Import-SpanTuiDotEnv -Root $worktreeRoot
        if ($env:SPANTUI_SELFTEST_SHARED -ne 'main') { throw 'A worktree must read the main checkout .env.' }
        if ($env:SPANTUI_SELFTEST_OWN -ne 'worktree') { throw "A worktree's own .env must win over the main checkout's." }
    } finally {
        foreach ($name in @('SPANTUI_SELFTEST_SHARED', 'SPANTUI_SELFTEST_OWN')) { [Environment]::SetEnvironmentVariable($name, $null) }
    }

    # Repository .env: KEY=VALUE lines, comments, optional quotes; a variable
    # the process already has wins over the file.
    $dotEnv = Join-Path $testRoot '.env'
    Set-Content -LiteralPath $dotEnv -Value @(
        '# comment'
        ''
        'SPANTUI_SELFTEST_PLAIN=C:\Qt'
        'SPANTUI_SELFTEST_QUOTED = "C:\Program Files\Qt"'
        "export SPANTUI_SELFTEST_SINGLE='x y'"
        'SPANTUI_SELFTEST_KEPT=from-file'
    )
    $parsed = Read-SpanTuiDotEnv -Path $dotEnv
    if ($parsed.Count -ne 4 -or $parsed['SPANTUI_SELFTEST_PLAIN'] -ne 'C:\Qt' -or
        $parsed['SPANTUI_SELFTEST_QUOTED'] -ne 'C:\Program Files\Qt' -or
        $parsed['SPANTUI_SELFTEST_SINGLE'] -ne 'x y') {
        throw ('Unexpected .env parse result: ' + (($parsed.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join '; '))
    }
    try {
        $env:SPANTUI_SELFTEST_KEPT = 'from-process'
        Import-SpanTuiDotEnv -Root $testRoot
        if ($env:SPANTUI_SELFTEST_KEPT -ne 'from-process') { throw '.env must not override a variable the process already has.' }
        if ($env:SPANTUI_SELFTEST_PLAIN -ne 'C:\Qt') { throw '.env values must reach the process environment.' }
    } finally {
        foreach ($name in $parsed.Keys) { [Environment]::SetEnvironmentVariable($name, $null) }
    }
    if ((Read-SpanTuiDotEnv -Path (Join-Path $testRoot 'missing.env')).Count -ne 0) { throw 'A missing .env must read as empty.' }
    Set-Content -LiteralPath $dotEnv -Value 'not a pair'
    Assert-Throws { Read-SpanTuiDotEnv -Path $dotEnv } 'A malformed .env line must fail with its line number.'

    # Linker: none chosen resolves to nothing; SPANTUI_LINKER_DIR names the
    # folder of ld.<name>.exe; a chosen linker that cannot be found fails.
    if ((Resolve-SpanTuiLinkerDirectory -Linker '' -LinkerDir '') -ne '') { throw 'No linker chosen must resolve to nothing.' }
    $linkerDir = Join-Path $testRoot 'llvm\bin'
    New-Item -ItemType Directory -Path $linkerDir -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $linkerDir 'ld.selftest.exe') -Value ''
    if ((Resolve-SpanTuiLinkerDirectory -Linker 'selftest' -LinkerDir $linkerDir) -ne $linkerDir) {
        throw 'SPANTUI_LINKER_DIR must name the folder of ld.<linker>.exe.'
    }
    Assert-Throws { Resolve-SpanTuiLinkerDirectory -Linker 'selftest' -LinkerDir $testRoot } `
        'A chosen linker missing from SPANTUI_LINKER_DIR must fail.'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output 'SpanTui toolchain self-test passed.'
