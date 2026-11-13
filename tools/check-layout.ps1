# Repository layout lint.
#
# Checks (docs/workflow/overview.md, repository hygiene):
#   * root    - every top-level entry git does not ignore (tracked or new) is
#               listed in the repository table of docs/workflow/overview.md
#               (the table after the `repo-layout` marker comment);
#   * link    - no junction or symbolic link inside the checkout (ignored
#               directories are checked themselves but not descended into);
#   * machine - no file git does not ignore contains this checkout's absolute
#               path or the current user's profile directory;
#   * define  - no compile definition in a CMake file is built from the
#               source or build directory or a target's file path: sccache
#               shares objects between checkouts (docs/workflow/build.md);
#   * bom     - every .ps1 with non-ASCII text starts with a UTF-8 BOM:
#               Windows PowerShell 5.1 reads a BOM-less script in the system
#               code page, which breaks it on machines with another locale.
#
# Usage:  tools\check-layout.ps1
# Exit:   0 clean, 1 violations found.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
$violations = [System.Collections.Generic.List[string]]::new()

function Add-Violation([string]$Rule, [string]$Location, [string]$Message) {
    $script:violations.Add(("{0}  [{1}] {2}" -f $Location, $Rule, $Message))
}

function Invoke-Git([string[]]$Arguments) {
    $output = & git -C $repoRoot @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed" }
    return @($output | Where-Object { $_ })
}

$layoutDoc = Join-Path $repoRoot 'docs/workflow/overview.md'
$allowed = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$inTable = $false
foreach ($line in (Get-Content -LiteralPath $layoutDoc -Encoding utf8)) {
    if ($line -match '<!--\s*repo-layout\s*-->') { $inTable = $true; continue }
    if (-not $inTable) { continue }
    if ($line -notmatch '^\s*\|') {
        if ($allowed.Count -gt 0) { break }
        continue
    }
    $cell = $line.Split('|')[1]
    foreach ($m in [regex]::Matches($cell, '`([^`]+)`')) { [void]$allowed.Add($m.Groups[1].Value) }
}
if ($allowed.Count -eq 0) { throw "No repo-layout table found in $layoutDoc" }

$visible = @(Invoke-Git @('ls-files')) + @(Invoke-Git @('ls-files', '--others', '--exclude-standard', '--directory'))
$roots = $visible | ForEach-Object {
    $first, $rest = $_.Split('/', 2)
    if ($rest -or $_.EndsWith('/')) { "$first/" } else { $first }
} | Sort-Object -Unique
foreach ($entry in $roots) {
    if (-not $allowed.Contains($entry)) {
        Add-Violation 'root' $entry 'not in the repository table of docs/workflow/overview.md: list it with its purpose, or move it under .dev/ or target/'
    }
}

foreach ($entry in (Invoke-Git @('ls-files', '-s'))) {
    if ($entry -match '^120000 \S+ \S+\t(.+)$') { Add-Violation 'link' $Matches[1] 'tracked symbolic link' }
}

$ignoredDirs = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($entry in (Invoke-Git @('ls-files', '--others', '--ignored', '--exclude-standard', '--directory'))) {
    if ($entry.EndsWith('/')) {
        [void]$ignoredDirs.Add([System.IO.Path]::GetFullPath((Join-Path $repoRoot $entry.TrimEnd('/'))))
    }
}
$gitDir = Join-Path $repoRoot '.git'
$pending = [System.Collections.Generic.Stack[System.IO.DirectoryInfo]]::new()
$pending.Push([System.IO.DirectoryInfo]::new($repoRoot))
while ($pending.Count -gt 0) {
    foreach ($child in $pending.Pop().EnumerateFileSystemInfos()) {
        $relative = $child.FullName.Substring($repoRoot.Length + 1).Replace('\', '/')
        if ($child.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            Add-Violation 'link' $relative 'junction or symbolic link: point SPANTUI_BUILD_ROOT / SPANTUI_ARTIFACT_ROOT in .env at the other location instead'
            continue
        }
        if (-not ($child -is [System.IO.DirectoryInfo])) { continue }
        if ($child.FullName -eq $gitDir -or $ignoredDirs.Contains($child.FullName)) { continue }
        $pending.Push($child)
    }
}

$needles = @($repoRoot, $env:USERPROFILE) | Where-Object { $_ } | ForEach-Object {
    $_.TrimEnd('\')
    $_.TrimEnd('\').Replace('\', '/')
    $_.TrimEnd('\').Replace('\', '\\')
} | Sort-Object -Unique
$grepArgs = @('grep', '--untracked', '-I', '-n', '-i', '-F')
foreach ($needle in $needles) { $grepArgs += @('-e', $needle) }
$hits = @(& git -C $repoRoot @grepArgs)
if ($LASTEXITCODE -gt 1) { throw 'git grep failed' }
# One line per file: the first hit and how many there are.
$byFile = [ordered]@{}
foreach ($hit in $hits) {
    if ($hit -match '^([^:]+):(\d+):') {
        if ($byFile.Contains($Matches[1])) { $byFile[$Matches[1]].Count++ }
        else { $byFile[$Matches[1]] = @{ Line = $Matches[2]; Count = 1 } }
    }
}
foreach ($file in $byFile.Keys) {
    $first = $byFile[$file]
    Add-Violation 'machine' ("{0}:{1}" -f $file, $first.Line) ("absolute path of this checkout or user profile ({0} line(s)): use a repository-relative path, .env or an environment placeholder" -f $first.Count)
}

# Compile definitions are hashed by sccache with the checkout and build tree
# stripped, so a path compiled into the code would reach another checkout.
$cmakeFiles = @(Invoke-Git @('ls-files', '--cached', '--others', '--exclude-standard', '--', 'CMakeLists.txt', '*/CMakeLists.txt', '*.cmake')) | Sort-Object -Unique
$pathExpression = '\$\{(CMAKE|PROJECT)_(CURRENT_)?(SOURCE|BINARY)_DIR\}|\$<TARGET_[A-Z_]*FILE[A-Z_]*:'
foreach ($file in $cmakeFiles) {
    $path = Join-Path $repoRoot $file
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $text = [System.IO.File]::ReadAllText($path)
    foreach ($call in [regex]::Matches($text, '(?is)\b(?:target_|add_)compile_definitions\s*\(([^)]*)\)')) {
        if ($call.Groups[1].Value -match $pathExpression) {
            $line = $text.Substring(0, $call.Index).Split("`n").Count
            Add-Violation 'define' ("{0}:{1}" -f $file, $line) 'compile definition carries a checkout or build-tree path: objects are shared between checkouts (build.md); embed the file as a resource or find it beside the executable at run time'
        }
    }
}

$scripts = @(Invoke-Git @('ls-files', '--cached', '--others', '--exclude-standard', '--', '*.ps1')) | Sort-Object -Unique
foreach ($script in $scripts) {
    $path = Join-Path $repoRoot $script
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    if (-not $hasBom -and @($bytes | Where-Object { $_ -gt 0x7F }).Count -gt 0) {
        Add-Violation 'bom' $script 'non-ASCII text without a UTF-8 BOM: save it as UTF-8 with BOM'
    }
}

if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Output $_ }
    Write-Output ("check-layout: {0} violation(s)" -f $violations.Count)
    exit 1
}
Write-Output 'check-layout: OK'
exit 0
