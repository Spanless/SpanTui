# `dev release`: packages the master commit CI tested in full and publishes it
# as the GitHub release vX.Y.Z of the version in CMakeLists.txt, once
# tools/check-version.ps1 passes. It runs on the maintainer's machine because
# the shell identity package is signed with the CN=Span certificate in the
# local certificate store, whose private key never leaves it
# (docs/workflow/release.md).
#
# -DryRun rehearses the whole flow on any commit: each gate that would stop a
# release is reported and the run goes on, the package is built and checked,
# and nothing is published; the gh commands are printed instead.
[CmdletBinding()]
param(
    [switch]$DryRun,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\toolchain.ps1')

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$version = Get-SpanTuiVersion (Join-Path $root 'CMakeLists.txt')
$tag = "v$version"
$blocked = [System.Collections.Generic.List[string]]::new()

function Invoke-Git {
    $output = & git -C $root @args
    if ($LASTEXITCODE -ne 0) { Fail "git $($args -join ' ') failed with exit code $LASTEXITCODE." }
    return $output
}

# A release gate: stops a release, only reported by a dry run.
function Stop-Release([string]$Message) {
    if (-not $DryRun) { Fail $Message }
    Write-Host "BLOCKED: $Message" -ForegroundColor Yellow
    $script:blocked.Add($Message)
}

$hasGh = [bool](Get-Command gh -ErrorAction SilentlyContinue)
if (-not $hasGh) { Stop-Release 'The GitHub CLI (gh) is required: https://cli.github.com, then gh auth login.' }

# Exactly what CI tested: master's head, nothing uncommitted.
$origin = [string](Invoke-Git remote get-url origin)
if ($origin -notmatch 'github\.com[:/]([^/]+/[^/]+?)(\.git)?/?$') { Fail "origin is not a GitHub repository: $origin" }
$repo = $Matches[1]
Invoke-Git fetch --quiet origin master | Out-Null
$head = [string](Invoke-Git rev-parse HEAD)
$master = [string](Invoke-Git rev-parse origin/master)
if ($head -ne $master) {
    Stop-Release "HEAD ($head) is not origin/master ($master); release from master's head, e.g. git switch --detach origin/master."
}
$dirty = @(Invoke-Git status --porcelain)
if ($dirty.Count -gt 0) { Stop-Release "The working tree has uncommitted changes:`n$($dirty -join "`n")" }

# Version gate: format, manifests, CHANGELOG section, and a tag above every
# release. It also writes the version's CHANGELOG section as the notes.
$notes = Join-Path (Get-SpanTuiDevPath -Name 'tmp' -Root $root) "release-notes-$version.md"
if (Test-Path -LiteralPath $notes) { Remove-Item -LiteralPath $notes -Force }
& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\check-version.ps1') -NotesOut $notes
if ($LASTEXITCODE -ne 0) { Stop-Release "Version $version is not ready to release; fix it on dev and merge it into master first." }

# The package must be built with the Qt and MinGW CI tested with
# (tools/toolchain-versions.json); discovery would otherwise take whatever
# newest kit this machine has.
$pinned = Get-Content -LiteralPath (Join-Path $root 'tools\toolchain-versions.json') -Raw | ConvertFrom-Json
$toolchain = Resolve-SpanTuiToolchain -RequireWindeployQt
$qtVersion = [string](& (Join-Path $toolchain.QtRoot 'bin\qmake.exe') -query QT_VERSION)
$gccVersion = [string](& $toolchain.Compiler -dumpfullversion)
if ($qtVersion -ne $pinned.qt) { Stop-Release "Qt $qtVersion at $($toolchain.QtRoot) is not the pinned Qt $($pinned.qt); set SPANTUI_QT_ROOT." }
if ($gccVersion -ne $pinned.mingwGcc) {
    Stop-Release "MinGW gcc $gccVersion at $($toolchain.MingwBin) is not the pinned $($pinned.mingwGcc) ($($pinned.mingwTool)); set SPANTUI_MINGW_BIN."
}
Write-Host "Toolchain matches CI: Qt $qtVersion, MinGW gcc $gccVersion"

# The push run on master ran the full suite, or skipped it because dev's push
# run passed it on the same tree (.github/workflows/ci.yml).
if ($hasGh) {
    $json = & gh run list --repo $repo --workflow ci.yml --commit $head --event push --json status,conclusion,url
    if ($LASTEXITCODE -ne 0) {
        Stop-Release 'Could not list the CI runs of this commit (gh auth status?).'
    } else {
        # Windows PowerShell emits a JSON array as one object; ForEach-Object unrolls it.
        $runs = @(($json -join "`n") | ConvertFrom-Json | ForEach-Object { $_ })
        if ($runs.Count -eq 0) {
            Stop-Release "No CI push run for $head yet."
        } elseif ($runs[0].status -ne 'completed' -or $runs[0].conclusion -ne 'success') {
            Stop-Release "CI for $head is $($runs[0].status)/$($runs[0].conclusion): $($runs[0].url)"
        } else {
            Write-Host "CI passed for ${head}: $($runs[0].url)"
        }
    }
}
$what = if ($DryRun) { 'Rehearsing' } else { 'Releasing' }
Write-Host "$what $tag from $head"

& (Join-Path $PSScriptRoot 'package-worker.ps1') -Jobs $Jobs
if ($LASTEXITCODE -ne 0) { Fail 'Packaging failed.' }

$output = Resolve-SpanTuiReleaseDirectory -Root $root -Version $version
$assets = @("SpanTui-$version-Setup.exe", "SpanTui-$version-portable.zip", 'release-manifest.json') |
    ForEach-Object { Join-Path $output $_ }
foreach ($asset in $assets) { Require-File $asset 'Release asset' }
# The binaries carry the version they were built with: the build stamp the
# update check compares and the manifest must both say what the tag says.
$built = @{
    'release-manifest.json' = (Get-Content -LiteralPath (Join-Path $output 'release-manifest.json') -Raw | ConvertFrom-Json).version
    'SpanTui-portable/spantui-build.json' = (Get-Content -LiteralPath (Join-Path $output 'SpanTui-portable\spantui-build.json') -Raw | ConvertFrom-Json).version
}
foreach ($file in $built.Keys) {
    if ($built[$file] -ne $version) { Fail "$file says version $($built[$file]), expected $version." }
}
Write-Host "Package checked: $version in release-manifest.json and spantui-build.json"

if ($DryRun) {
    Write-Host 'Dry run: would now run'
    Write-Host "  gh release create $tag <Setup.exe, portable.zip, release-manifest.json> --repo $repo --draft --target $head --title `"SpanTui $version`" --notes-file $notes"
    Write-Host "  gh release edit $tag --repo $repo --draft=false --latest"
    if ($blocked.Count -gt 0) {
        Write-Host "DRY_RUN_BLOCKED: a real release would stop at $($blocked.Count) gate(s) above; the package itself is in $output" -ForegroundColor Yellow
        exit 1
    }
    Write-Host "DRY_RUN_OK: $tag is ready to release ($output)" -ForegroundColor Green
    exit 0
}

# Created as a draft and published once every asset is up, so a failed upload
# never leaves a half-filled release as the latest one. Publishing creates the
# tag on the tested commit; the notes are the version's CHANGELOG section.
Invoke-SpanTuiTimedStep "Publish GitHub release $tag" {
    & gh release create $tag @assets --repo $repo --draft --target $head --title "SpanTui $version" --notes-file $notes
    if ($LASTEXITCODE -ne 0) { Fail "Could not create the draft release $tag; delete a leftover draft of it if there is one." }
    & gh release edit $tag --repo $repo --draft=false --latest
    if ($LASTEXITCODE -ne 0) { Fail "The draft release $tag is uploaded but not published; publish it on GitHub." }
}
Write-Host "RELEASE_OK: https://github.com/$repo/releases/tag/$tag" -ForegroundColor Green
exit 0
