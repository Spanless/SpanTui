# `dev doctor`: what the tooling finds on this machine, without building
# anything. The required toolchain (Qt kit, MinGW, CMake, Ninja) decides the
# result; optional tools are only reported, each with what it is for.
# Output ends with DOCTOR_OK or DOCTOR_FAIL.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\toolchain.ps1')

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$checkout = Get-SpanTuiCheckout -Root $root
$pinned = Get-Content -LiteralPath (Join-Path $root 'tools\toolchain-versions.json') -Raw | ConvertFrom-Json

function Write-Row([string]$Label, [string]$Value, [string]$Note = '', [string]$Color = 'Gray') {
    Write-Host ('  {0,-13} {1}' -f $Label, $Value) -ForegroundColor $Color
    if ($Note) { Write-Host ('  {0,-13} {1}' -f '', $Note) -ForegroundColor DarkGray }
}

function Get-FirstLine([string]$Exe, [string[]]$Arguments) {
    try { return [string](@(& $Exe @Arguments 2>&1) | Select-Object -First 1) } catch { return '' }
}

Write-Host 'Settings'
$envFiles = @(Join-Path $checkout.Root '.env')
if ($checkout.IsWorktree) { $envFiles += Join-Path $checkout.MainRoot '.env' }
foreach ($file in $envFiles) {
    $state = if (Test-Path -LiteralPath $file -PathType Leaf) { 'read' } else { 'absent (defaults apply; template: .env.example)' }
    Write-Row '.env' "$file  $state"
}
Write-Row 'checkout' "$($checkout.Name)  $($checkout.Root)"
Write-Row 'build tree' (Resolve-SpanTuiBuildDirectory -Root $root)
Write-Row 'worktrees' (Resolve-SpanTuiWorktreeRoot -Root $root) 'dev worktree add <branch> creates them here'

Write-Host 'Toolchain (required)'
$failed = $false
$toolchain = $null
try {
    $toolchain = Resolve-SpanTuiToolchain
} catch {
    # Fail has printed what is missing and which variable fixes it.
    $failed = $true
}
if ($toolchain) {
    $qtVersion = Split-Path -Leaf (Split-Path -Parent $toolchain.QtRoot)
    $qtNote = if ($qtVersion -ne $pinned.qt) { "CI and releases use Qt $($pinned.qt)" } else { '' }
    Write-Row 'Qt' "$qtVersion  $($toolchain.QtRoot)" $qtNote
    $gccVersion = Get-FirstLine $toolchain.Compiler @('-dumpfullversion')
    $gccNote = if ($gccVersion -ne $pinned.mingwGcc) { "CI and releases use MinGW $($pinned.mingwGcc)" } else { '' }
    Write-Row 'MinGW' "$gccVersion  $($toolchain.MingwBin)" $gccNote
    Write-Row 'CMake' ("{0}  {1}" -f ((Get-FirstLine $toolchain.CMake @('--version')) -replace '^cmake version ', ''), $toolchain.CMake)
    Write-Row 'Ninja' ("{0}  {1}" -f (Get-FirstLine $toolchain.Ninja @('--version')), $toolchain.Ninja)
}

Write-Host 'Optional'
$sccache = Get-Command sccache.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $sccache) {
    Write-Row 'sccache' 'not found' 'Builds compile without a cache. With sccache on PATH every checkout shares one compiler cache.' 'Yellow'
} elseif ($env:SPANTUI_SCCACHE -eq 'external') {
    Write-Row 'sccache' "$($sccache.Source)" 'SPANTUI_SCCACHE=external: the environment configures sccache, the tooling does not.'
} else {
    $cache = Get-SpanTuiCompilerCacheConfig -Root $root
    Write-Row 'sccache' ("{0}  {1}" -f ((Get-FirstLine $sccache.Source @('--version')) -replace '^sccache ', ''), $sccache.Source) `
        ("Shared by {0} checkout(s): port {1}, cache {2}" -f $cache.Checkouts.Count, $cache.Port, $cache.Directory)
}
if ([string]::IsNullOrWhiteSpace($env:SPANTUI_LINKER)) {
    $lld = Get-Command ld.lld.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $lld -and $env:ProgramFiles -and (Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'LLVM\bin\ld.lld.exe'))) { $lld = $true }
    $linkerNote = if ($lld) { 'ld.lld is installed: SPANTUI_LINKER=lld in .env links much faster.' } else { '' }
    Write-Row 'linker' 'GNU ld (SPANTUI_LINKER unset)' $linkerNote
} else {
    try {
        Write-Row 'linker' "$env:SPANTUI_LINKER  $(Resolve-SpanTuiLinkerDirectory)"
    } catch {
        $failed = $true
    }
}
$clangFormat = Get-Command clang-format.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
$clangFormatPath = if ($clangFormat) { $clangFormat.Source } elseif ($env:ProgramFiles) { Join-Path $env:ProgramFiles 'LLVM\bin\clang-format.exe' } else { '' }
if ($clangFormatPath -and (Test-Path -LiteralPath $clangFormatPath -PathType Leaf)) {
    $clangFormatVersion = (Get-FirstLine $clangFormatPath @('--version')) -replace '^.*version (\S+).*$', '$1'
    $clangFormatNote = if ($clangFormatVersion -ne $pinned.clangFormat) { "CI pins $($pinned.clangFormat); another major version may format differently." } else { '' }
    Write-Row 'clang-format' "$clangFormatVersion  $clangFormatPath" $clangFormatNote
} else {
    Write-Row 'clang-format' 'not found' 'The pre-commit hook skips the C++ format check (WARN).' 'Yellow'
}
$hooks = [string](& git -C $root config --get core.hooksPath)
if ($hooks -eq '.githooks') {
    Write-Row 'git hooks' '.githooks'
} else {
    Write-Row 'git hooks' 'not enabled' 'git config core.hooksPath .githooks  (pre-commit runs the static checks)' 'Yellow'
}
Write-Row 'packaging' 'checked by dev package' 'Inno Setup, Windows SDK and the signing certificate; dev package --quick needs none of them.'

if ($failed) {
    Write-Host 'DOCTOR_FAIL' -ForegroundColor Red
    exit 1
}
Write-Host 'DOCTOR_OK' -ForegroundColor Green
exit 0
