# Static gate for a change (no build, no tests).
#
# Runs the static checks that apply to the changed files and prints the
# ownership guidance for them:
#   * tools/check-style.ps1      when qml/ changed;
#   * tools/check-translations.ps1  when qml/, src/ or translations/ changed;
#   * tools/check-icons.ps1      when qml/ or assets/icons/ changed;
#   * tools/check-format.ps1     when C++ changed: clang-format on the
#                                changed lines (WARN when it is not installed);
#   * tools/check-docs.ps1       always (also validates the ownership map);
#   * tools/check-layout.ps1     always (root entries, links, machine paths);
#   * tools/check-ownership.ps1  -Changed: docs to read/sync, checks to run;
#   * git diff --check           whitespace / conflict markers.
# Nothing here builds, runs or tests the product (see docs/workflow/verification.md).
#
# Usage:  tools\agent-check.ps1 [-Staged]
# Exit:   0 clean, 1 when any check failed.
[CmdletBinding()]
param([switch]$Staged)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
$failed = [System.Collections.Generic.List[string]]::new()

# Each check runs in its own process so its strictness and exit code stay
# its own.
function Invoke-Check([string]$Script, [string[]]$CheckArgs = @()) {
    & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $Script) @CheckArgs
}

function Invoke-Step([string]$Name, [scriptblock]$Action) {
    Write-Output ">> $Name"
    & $Action
    if ($LASTEXITCODE -ne 0) { $script:failed.Add($Name) }
}

$changed = if ($Staged) {
    @(& git -C $repoRoot diff --cached --name-only)
} else {
    @(& git -C $repoRoot diff --name-only HEAD) + @(& git -C $repoRoot ls-files --others --exclude-standard)
}
$changed = @($changed | Where-Object { $_ } | Sort-Object -Unique)
if ($changed.Count -eq 0) {
    Write-Output 'No changes.'
    exit 0
}

if (@($changed | Where-Object { $_ -like 'qml/*' }).Count -gt 0) {
    Invoke-Step 'QML style' { Invoke-Check 'check-style.ps1' }
}
if (@($changed | Where-Object { $_ -like 'qml/*' -or $_ -like 'src/*' -or $_ -like 'translations/*' }).Count -gt 0) {
    Invoke-Step 'Translations' { Invoke-Check 'check-translations.ps1' }
}
if (@($changed | Where-Object { $_ -like 'qml/*' -or $_ -like 'assets/icons/*' }).Count -gt 0) {
    Invoke-Step 'Icons' { Invoke-Check 'check-icons.ps1' }
}
if (@($changed | Where-Object { $_ -match '\.(h|hpp|cpp|cc)$' }).Count -gt 0) {
    $formatArgs = if ($Staged) { @('-Staged') } else { @() }
    Invoke-Step 'C++ format' { Invoke-Check 'check-format.ps1' $formatArgs }
}
Invoke-Step 'Docs' { Invoke-Check 'check-docs.ps1' }
Invoke-Step 'Layout' { Invoke-Check 'check-layout.ps1' }
$ownershipArgs = if ($Staged) { @('-Changed', '-Staged') } else { @('-Changed') }
Invoke-Step 'Ownership' { Invoke-Check 'check-ownership.ps1' $ownershipArgs }
Invoke-Step 'Whitespace' {
    if ($Staged) { & git -C $repoRoot diff --cached --check } else { & git -C $repoRoot diff HEAD --check }
}

if ($failed.Count -gt 0) {
    Write-Output "`nAGENT_CHECK_FAIL: $($failed -join ', ')"
    exit 1
}
Write-Output "`nAGENT_CHECK_OK"
exit 0
