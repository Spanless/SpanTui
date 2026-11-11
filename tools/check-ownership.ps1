# Ownership map (docs/ownership.json) gate.
#
# Validation (always):
#   * every doc named in the map exists;
#   * every tracked or new file under the governed roots matches an entry;
#   * every highImpact path matches at least one file.
#
# Guidance (-Files a,b before editing, or -Changed [-Staged] after): for
# those files, print the docs to read, the high-impact objects touched, the
# entry checks; with -Changed also warn when owned code changed while none of
# its docs did.
# Warnings never fail the run.
#
# Usage:  tools\check-ownership.ps1 [-Files <paths> | -Changed [-Staged]] [-Quiet]
# Exit:   0 consistent, 1 violations found.
[CmdletBinding()]
param(
    [switch]$Changed,
    [switch]$Staged,
    [string[]]$Files = @(),
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
$map = Get-Content -LiteralPath (Join-Path $repoRoot 'docs/ownership.json') -Raw -Encoding utf8 | ConvertFrom-Json
$entries = @($map.entries)
$violations = [System.Collections.Generic.List[string]]::new()

function Get-Entries([string]$Path) {
    return @($entries | Where-Object {
        $entry = $_
        @($entry.paths | Where-Object { $Path -like $_ }).Count -gt 0
    })
}

function Invoke-Git([string[]]$GitArgs) {
    $out = & git -C $repoRoot @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed" }
    return @($out | Where-Object { $_ })
}

foreach ($entry in $entries) {
    foreach ($doc in $entry.docs) {
        if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $doc))) {
            $violations.Add("ownership.json  [doc] entry '$($entry.name)' names missing $doc")
        }
    }
}

$governed = '^(src|qml|tests|tools|installer|shaders|assets|translations|docs)/|^[^/]+\.(bat|md)$|^CMakeLists\.txt$'
foreach ($file in (Invoke-Git @('ls-files', '--cached', '--others', '--exclude-standard'))) {
    if ($file -notmatch $governed) { continue }
    if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $file))) { continue }   # deleted in the working tree
    if (@(Get-Entries $file).Count -eq 0) {
        $violations.Add("ownership.json  [coverage] no entry owns $file")
    }
}

foreach ($item in $map.highImpact) {
    if (@(Get-ChildItem -Path (Join-Path $repoRoot $item.path) -ErrorAction SilentlyContinue).Count -eq 0) {
        $violations.Add("ownership.json  [highImpact] '$($item.object)' path matches no file: $($item.path)")
    }
}

if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Output $_ }
    if (-not $Quiet) { Write-Output "`nOwnership check FAILED: $($violations.Count) violation(s)." }
    exit 1
}
if (-not $Changed -and $Files.Count -eq 0) {
    if (-not $Quiet) { Write-Output 'Ownership check passed.' }
    exit 0
}

if ($Files.Count -eq 0) {
    $Files = if ($Staged) {
        Invoke-Git @('diff', '--cached', '--name-only')
    } else {
        @(Invoke-Git @('diff', '--name-only', 'HEAD')) + @(Invoke-Git @('ls-files', '--others', '--exclude-standard'))
    }
}
# powershell.exe -File passes "a,b" as one string; split it here.
$Files = @($Files | ForEach-Object { $_ -split ',' } | Where-Object { $_ } | ForEach-Object { $_.Trim().Replace('\', '/') } | Sort-Object -Unique)
$read = [System.Collections.Generic.SortedSet[string]]::new()
$checks = [System.Collections.Generic.SortedSet[string]]::new()
$warnings = [System.Collections.Generic.List[string]]::new()
foreach ($entry in $entries) {
    $hits = @($Files | Where-Object { $f = $_; @($entry.paths | Where-Object { $f -like $_ }).Count -gt 0 })
    if ($hits.Count -eq 0) { continue }
    foreach ($doc in $entry.docs) { [void]$read.Add($doc) }
    foreach ($check in $entry.checks) { [void]$checks.Add($check) }
    $codeHits = @($hits | Where-Object { $_ -notlike 'docs/*' -and $_ -notlike '*.md' })
    $docTouched = @($entry.docs | Where-Object { $Files -contains $_ }).Count -gt 0
    if ($Changed -and $codeHits.Count -gt 0 -and -not $docTouched) {
        $warnings.Add("WARN  '$($entry.name)' changed ($($codeHits.Count) file(s)) but none of its docs did: $($entry.docs -join ', ') -- sync them if documented behavior changed, otherwise state why not in the handoff")
    }
}
$impact = @($map.highImpact | Where-Object { $p = $_.path; @($Files | Where-Object { $_ -like $p }).Count -gt 0 })
Write-Output "Files: $($Files.Count)"
Write-Output 'Docs to read / keep in sync:'
$read | ForEach-Object { Write-Output "  $_" }
if ($impact.Count -gt 0) {
    Write-Output 'High-impact (search all callers before changing API or behavior):'
    $impact | ForEach-Object { Write-Output "  $($_.object)  ($($_.path))" }
}
# Group the entry checks by stage (docs/workflow/verification.md): static
# checks run any time; the final verification builds once, runs one merged
# regression pattern and, when listed, the smoke run and other build scripts
# (e.g. the stress TUI); the rest needs approval. Only lint scripts and git
# checks are static.
$static = @($checks | Where-Object { $_ -like 'tools/check-*.ps1' -or $_ -like 'git *' })
$finalScripts = @($checks | Where-Object { $_ -notlike 'dev.bat *' -and $static -notcontains $_ })
$patterns = [System.Collections.Generic.SortedSet[string]]::new()
$placeholders = [System.Collections.Generic.SortedSet[string]]::new()
foreach ($check in $checks) {
    if ($check -notmatch '^dev\.bat test (-R .+)$') { continue }
    $selectors = $Matches[1]
    if ($selectors.Contains('<')) { [void]$placeholders.Add(($selectors -replace '^-R\s*', '').Trim('<', '>')); continue }
    foreach ($match in [regex]::Matches($selectors, '-R\s+(\S+)')) { [void]$patterns.Add($match.Groups[1].Value) }
}
$needsBuild = @($checks | Where-Object { $_ -like 'dev.bat *' -and $_ -ne 'dev.bat package' }).Count -gt 0
Write-Output 'Static checks (any time):'
$static | ForEach-Object { Write-Output "  $_" }
if ($needsBuild -or $finalScripts.Count -gt 0) {
    Write-Output 'Final verification (once, when the task is done):'
}
if ($needsBuild) {
    Write-Output '  dev.bat build'
    if ($patterns.Count -gt 0) {
        $extra = if ($placeholders.Count -gt 0) { "   + tests for: $($placeholders -join ', ')" } else { '' }
        # One -R per pattern (dev test ORs them): a quoted "a|b" loses its
        # quotes when PowerShell launches dev.bat and cmd reads `|` as a pipe.
        Write-Output "  dev.bat test $(($patterns | ForEach-Object { "-R $_" }) -join ' ')$extra"
    } elseif ($placeholders.Count -gt 0) {
        Write-Output "  dev.bat test -R <tests for: $($placeholders -join ', ')>"
    }
    if ($checks.Contains('dev.bat run --smoke')) { Write-Output '  dev.bat run --smoke' }
}
$finalScripts | ForEach-Object { Write-Output "  $_" }
if ($checks.Contains('dev.bat package')) {
    Write-Output 'Needs approval:'
    Write-Output '  dev.bat package'
}
# A planned path that is not on disk is a new file or a typo; say which.
if (-not $Changed) {
    $absent = @($Files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $repoRoot $_)) })
    if ($absent.Count -gt 0) {
        Write-Output 'Not on disk (new file, or check the path):'
        $absent | ForEach-Object { Write-Output "  $_" }
    }
    $unowned = @($Files | Where-Object { @(Get-Entries $_).Count -eq 0 })
    if ($unowned.Count -gt 0) {
        Write-Output 'No ownership entry (add one to docs/ownership.json if the file is new):'
        $unowned | ForEach-Object { Write-Output "  $_" }
    }
}
$warnings | ForEach-Object { Write-Output $_ }
exit 0
