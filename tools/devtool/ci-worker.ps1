# `dev check` / `dev ci`: the gate GitHub CI runs (.github/workflows/ci.yml),
# runnable locally. `check` runs the lint scripts and needs no toolchain;
# `ci` runs them, then an incremental build and the full test suite. With
# -Core (pull requests into dev) the tests are the ones labelled core plus the
# ones the changes since -Base touch. The pre-push hook (.githooks/pre-push)
# runs `dev check`.
[CmdletBinding()]
param(
    [switch]$StaticOnly,
    [switch]$Core,
    # Revision the changes are measured from (working tree included); defaults
    # to the merge base with origin/dev.
    [string]$Base = '',
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$tools = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $tools

# Libraries most others link (src/*/CMakeLists.txt): the ownership checks name
# a few tests for them, but a change there can break any test.
$foundation = @('src/core/*', 'src/platform/*')

# The regex of the tests a change set touches: the `dev.bat test -R ...`
# checks of every docs/ownership.json entry owning a changed file. $null when
# a change can reach any test: foundation libraries, build scripts and tools
# (`<affected tests>`), or test support files other than one test's own source.
function Get-AffectedTestRegex([string[]]$Files) {
    $map = Get-Content -LiteralPath (Join-Path $root 'docs\ownership.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $patterns = [System.Collections.Generic.SortedSet[string]]::new()
    foreach ($file in $Files) {
        if (@($foundation | Where-Object { $file -like $_ }).Count -gt 0) { return $null }
        foreach ($entry in $map.entries) {
            if (@($entry.paths | Where-Object { $file -like $_ }).Count -eq 0) { continue }
            foreach ($check in $entry.checks) {
                if ($check -notmatch '^dev\.bat test (-R .+)$') { continue }
                $selectors = $Matches[1]
                if ($selectors -eq '-R <changed test>' -and $file -match '^tests/.*/([^/]+_test)\.cpp$') {
                    [void]$patterns.Add("^$($Matches[1])$")
                } elseif ($selectors.Contains('<')) {
                    return $null
                } else {
                    foreach ($match in [regex]::Matches($selectors, '-R\s+(\S+)')) { [void]$patterns.Add($match.Groups[1].Value) }
                }
            }
        }
    }
    return ($patterns -join '|')
}

foreach ($check in @('check-docs.ps1', 'check-layout.ps1', 'check-style.ps1')) {
    Write-Host ">> $check"
    & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools $check)
    if ($LASTEXITCODE -ne 0) {
        Write-Host "CI_FAIL: $check"
        exit 1
    }
}
if ($StaticOnly) {
    Write-Host 'CHECK_OK'
    exit 0
}

# Decided before the long build so a bad base fails at once. Native stderr
# is not redirected: under Stop, Windows PowerShell turns it into an error.
$testArgs = @{ Jobs = $Jobs }
$testLabel = 'full suite'
if ($Core) {
    if (-not $Base) {
        $Base = [string](& git -C $root merge-base origin/dev HEAD)
        if ($LASTEXITCODE -ne 0 -or -not $Base) {
            Write-Host 'CI_FAIL: no merge base with origin/dev; pass --base <revision>'
            exit 1
        }
    }
    $files = @(& git -C $root diff --name-only $Base)
    if ($LASTEXITCODE -ne 0) {
        Write-Host "CI_FAIL: cannot diff against $Base"
        exit 1
    }
    $files += @(& git -C $root ls-files --others --exclude-standard)
    $affected = Get-AffectedTestRegex $files
    Write-Host ("Changed since {0}: {1} file(s)" -f $Base, $files.Count)
    if ($null -eq $affected) {
        $testLabel = 'full suite: the changes reach foundation libraries, build scripts, tools or test support'
    } else {
        $testArgs.Label = 'core'
        if ($affected) { $testArgs.Regex = $affected }
        $testLabel = if ($affected) { "core + affected: $affected" } else { 'core' }
    }
}

Write-Host '>> build'
& (Join-Path $PSScriptRoot 'start-worker.ps1') -SkipLaunch -Jobs $Jobs
if ($LASTEXITCODE -ne 0) {
    Write-Host 'CI_FAIL: build'
    exit 1
}
Write-Host ">> test ($testLabel)"
& (Join-Path $PSScriptRoot 'test-worker.ps1') @testArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host 'CI_FAIL: test'
    exit 1
}
Write-Host 'CI_OK'
exit 0
