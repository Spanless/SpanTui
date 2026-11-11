# Release version gate (docs/workflow/release.md).
#
# The version is project(SpanTui VERSION X.Y.Z) in CMakeLists.txt. A release
# of it requires:
#   * format   - X.Y.Z, three numbers;
#   * manifest - the assemblyIdentity of every application manifest in
#                assets/brand is X.Y.Z.0 (windres embeds them as they are);
#   * notes    - CHANGELOG.md has a `## [X.Y.Z]` section and nothing left
#                under `## [Unreleased]`;
#   * tag      - origin has no tag vX.Y.Z, and X.Y.Z is above every stable
#                release tag: the update check only offers a higher version.
# Runs on the dev -> master pull request (CI `version` job) and in
# `dev release`. Needs git and read access to origin; no build toolchain, so
# it also runs on the Linux runner.
#
# Usage:  tools\check-version.ps1 [-NotesOut <file>]
#         -NotesOut writes the version's CHANGELOG section (the release notes).
# Exit:   0 ready to release, 1 violations found.
[CmdletBinding()]
param(
    [string]$NotesOut = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
$violations = [System.Collections.Generic.List[string]]::new()

$cmake = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Encoding UTF8
$match = $cmake | Select-String -Pattern '^project\(SpanTui\s+VERSION\s+(\S+)' | Select-Object -First 1
if (-not $match) {
    Write-Host 'VERSION_FAIL: no project(SpanTui VERSION ...) in CMakeLists.txt'
    exit 1
}
$version = $match.Matches[0].Groups[1].Value
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Host "VERSION_FAIL: '$version' in CMakeLists.txt is not X.Y.Z"
    exit 1
}
$escaped = [regex]::Escape($version)

# format/manifest
foreach ($manifest in @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'assets/brand') -Filter '*.manifest' -File)) {
    $text = Get-Content -LiteralPath $manifest.FullName -Raw -Encoding UTF8
    $identity = [regex]::Match($text, '<assemblyIdentity\b[^>]*?\bversion="([^"]+)"')
    if (-not $identity.Success) {
        $violations.Add("assets/brand/$($manifest.Name): no assemblyIdentity version")
    } elseif ($identity.Groups[1].Value -ne "$version.0") {
        $violations.Add("assets/brand/$($manifest.Name): assemblyIdentity version $($identity.Groups[1].Value), expected $version.0")
    }
}

# notes
$changelog = @(Get-Content -LiteralPath (Join-Path $repoRoot 'CHANGELOG.md') -Encoding UTF8)
$headings = @(for ($i = 0; $i -lt $changelog.Count; ++$i) { if ($changelog[$i] -match '^## ') { $i } })
function Get-Section([string]$Pattern) {
    foreach ($index in $headings) {
        if ($changelog[$index] -notmatch $Pattern) { continue }
        $next = @($headings | Where-Object { $_ -gt $index } | Select-Object -First 1)
        $end = if ($next.Count -gt 0) { $next[0] } else { $changelog.Count }
        return , @(if ($end -gt $index + 1) { $changelog[($index + 1)..($end - 1)] })
    }
    return $null
}
$unreleased = Get-Section '^## \[Unreleased\]'
if ($null -ne $unreleased -and @($unreleased | Where-Object { $_.Trim() }).Count -gt 0) {
    $violations.Add("CHANGELOG.md: entries are still under ## [Unreleased]; move them under ## [$version] (raise the version first if $version is already released)")
}
$notes = Get-Section "^## \[$escaped\]"
if ($null -eq $notes) {
    $violations.Add("CHANGELOG.md: no ## [$version] section")
} elseif (@($notes | Where-Object { $_.Trim() }).Count -eq 0) {
    $violations.Add("CHANGELOG.md: the ## [$version] section is empty")
}

# tag
$refs = @(& git -C $repoRoot ls-remote --tags origin 'refs/tags/v*')
if ($LASTEXITCODE -ne 0) {
    $violations.Add('origin: cannot list tags (git ls-remote failed)')
} else {
    $tags = @($refs | ForEach-Object { ($_ -split '\s+')[1] -replace '^refs/tags/', '' -replace '\^\{\}$', '' } | Sort-Object -Unique)
    $stable = @($tags | Where-Object { $_ -match '^v\d+\.\d+\.\d+$' } | ForEach-Object { [version]($_.Substring(1)) } | Sort-Object)
    if ($tags -contains "v$version") {
        $violations.Add("origin: v$version is already released; raise project(SpanTui VERSION ...) in CMakeLists.txt")
    } elseif ($stable.Count -gt 0 -and [version]$version -le $stable[-1]) {
        $violations.Add("origin: $version is not above the latest stable release v$($stable[-1])")
    }
}

if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Host "  $_" }
    Write-Host "VERSION_FAIL: $version is not ready to release ($($violations.Count) violation(s))"
    exit 1
}
if ($NotesOut) {
    $trimmed = (($notes -join "`n").Trim()) + "`n"
    [System.IO.File]::WriteAllText($NotesOut, $trimmed, [System.Text.UTF8Encoding]::new($false))
}
Write-Host "VERSION_OK: $version"
exit 0
