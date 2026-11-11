# clang-format over the C++ lines a change touches.
#
# Existing code is not reformatted in bulk: only added or modified lines of
# changed .h/.cpp files are checked against .clang-format. Third-party code
# under src/terminal/vendor/ is skipped.
#
# Usage:  tools\check-format.ps1 [-Staged | -Base <revision>] [-Fix] [-Require]
#   (none)   working tree changes since HEAD, new files included
#   -Staged  the staged content (pre-commit hook via agent-check -Staged)
#   -Base    changes since <revision> (CI: the pull request's base)
#   -Fix     rewrite those lines in the working tree instead of reporting
#   -Require fail when clang-format is missing (CI); otherwise a WARN
# The clang-format version CI pins is in tools/toolchain-versions.json; a
# local one of another major version may format differently and is reported.
# Exit:   0 clean, 1 lines need formatting (or clang-format missing with -Require).
[CmdletBinding()]
param(
    [switch]$Staged,
    [string]$Base = '',
    [switch]$Fix,
    [switch]$Require
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
if ($Staged -and $Base) { throw '-Staged and -Base exclude each other.' }
if ($Staged -and $Fix) { throw '-Fix rewrites the working tree; use it without -Staged.' }

function Invoke-Git([string[]]$Arguments) {
    $output = & git -C $repoRoot @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed" }
    return @($output | Where-Object { $_ })
}

$clangFormat = (Get-Command clang-format -ErrorAction SilentlyContinue | Select-Object -First 1)
$clangFormat = if ($clangFormat) { $clangFormat.Source } else {
    $candidate = Join-Path $env:ProgramFiles 'LLVM\bin\clang-format.exe'
    if (Test-Path -LiteralPath $candidate) { $candidate }
}
if (-not $clangFormat) {
    if ($Require) {
        Write-Output 'FAIL: clang-format not found (PATH or %ProgramFiles%\LLVM\bin).'
        exit 1
    }
    Write-Output 'WARN: clang-format not found (PATH or %ProgramFiles%\LLVM\bin); C++ formatting not checked.'
    exit 0
}

$pinned = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain-versions.json') -Raw | ConvertFrom-Json).clangFormat
$versionLine = [string](& $clangFormat --version)
if ($versionLine -match 'version (\d+)\.' -and $Matches[1] -ne $pinned.Split('.')[0]) {
    Write-Output "WARN: $versionLine differs from the pinned clang-format $pinned; results may differ from CI."
}

$diffArgs = if ($Staged) { @('--cached') } elseif ($Base) { @($Base) } else { @('HEAD') }
$files = @(Invoke-Git (@('diff', '--name-only', '--diff-filter=ACMR') + $diffArgs))
$untracked = @()
if (-not $Staged -and -not $Base) { $untracked = @(Invoke-Git @('ls-files', '--others', '--exclude-standard')) }
$files = @(($files + $untracked) | Where-Object { $_ -match '\.(h|hpp|cpp|cc)$' -and $_ -notlike 'src/terminal/vendor/*' } |
    Sort-Object -Unique)
if ($files.Count -eq 0) {
    Write-Output 'No C++ changes.'
    exit 0
}

# Staged content is checked from a copy, so unstaged edits do not count.
$stageRoot = Join-Path $repoRoot '.dev\tmp\format-check'
if ($Staged) {
    if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
    New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
}

$unformatted = [System.Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    $lines = [System.Collections.Generic.List[string]]::new()
    if ($untracked -contains $file) {
        $lines.Add('1:' + [int]::MaxValue)
    } else {
        foreach ($hunk in (Invoke-Git (@('diff', '-U0') + $diffArgs + @('--', $file)))) {
            if ($hunk -match '^@@ -\S+ \+(\d+)(?:,(\d+))? @@') {
                $start = [int]$Matches[1]
                $count = if ($Matches[2]) { [int]$Matches[2] } else { 1 }
                if ($count -gt 0) { $lines.Add("${start}:$($start + $count - 1)") }
            }
        }
    }
    if ($lines.Count -eq 0) { continue }

    $target = Join-Path $repoRoot $file
    if ($Staged) {
        Invoke-Git @('checkout-index', "--prefix=.dev/tmp/format-check/", '--', $file) | Out-Null
        $target = Join-Path $stageRoot $file
    }
    $lineArgs = @($lines | ForEach-Object { "--lines=$_" })
    # Native stderr under Stop would end the script at the first finding.
    $ErrorActionPreference = 'Continue'
    if ($Fix) {
        & $clangFormat --style=file -i @lineArgs $target
    } else {
        $report = @(& $clangFormat --style=file --dry-run --Werror @lineArgs $target 2>&1 | ForEach-Object { "$_" })
    }
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($Fix) {
        if ($exitCode -ne 0) { throw "clang-format failed on $file" }
        continue
    }
    if ($exitCode -ne 0) {
        $unformatted.Add($file)
        $report | Where-Object { $_ -match ': (error|warning):' } | Select-Object -First 5 |
            ForEach-Object { Write-Output ("  " + $_.Replace($stageRoot + '\', '').Replace($repoRoot + '\', '')) }
    }
}
if ($Staged) { Remove-Item -LiteralPath $stageRoot -Recurse -Force -ErrorAction SilentlyContinue }

if ($Fix) {
    Write-Output "FORMAT_FIXED: $($files.Count) file(s)"
    exit 0
}
if ($unformatted.Count -gt 0) {
    Write-Output "FORMAT_FAIL: changed lines need clang-format in $($unformatted.Count) file(s): $($unformatted -join ', ')"
    Write-Output '  Fix them with: tools\check-format.ps1 -Fix (working tree changes since HEAD)'
    exit 1
}
Write-Output "FORMAT_OK: $($files.Count) file(s)"
exit 0
