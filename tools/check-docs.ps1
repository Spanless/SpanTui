# Documentation consistency lint.
#
# Checks (docs/workflow/documentation.md, acceptance checklist):
#   * links    — every relative Markdown link in AGENTS.md, README.md and
#                docs/ resolves; #anchors match a heading slug of the target;
#   * lines    — no source line numbers (`file.cpp:94`, `file.qml:20-63`):
#                they drift with every edit; cite the file and symbol;
#   * paths    — `src/…`, `qml/…`, `tests/…`, `tools/…` paths quoted in docs
#                exist (`a.h/cpp` and `name.*` notations expand; paths with
#                `<`, `|`, `...` or `…` are placeholders and skipped);
#   * backrefs — every docs/*.md path mentioned in src/, qml/, tests/, tools/
#                and CMakeLists.txt exists;
#   * index    — docs/index.md links every doc;
#   * vendor   — the SHA-256 a docs/legal page records for a vendored binary is
#                the hash of the committed file.
#
# Usage:  tools\check-docs.ps1
# Exit:   0 clean, 1 violations found.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
$violations = [System.Collections.Generic.List[string]]::new()

function Add-Violation([string]$Rule, [string]$Location, [string]$Message) {
    $script:violations.Add(("{0}  [{1}] {2}" -f $Location, $Rule, $Message))
}

function Get-RelativePath([string]$Path) {
    return $Path.Substring($repoRoot.Length + 1).Replace('\', '/')
}

function Remove-CodeFences([string]$Text) {
    return [regex]::Replace($Text, '(?ms)^\s*```.*?^\s*```[^\n]*$', '')
}

function Get-HeadingSlugs([string]$Path) {
    $slugs = [System.Collections.Generic.HashSet[string]]::new()
    $text = Remove-CodeFences (Get-Content -LiteralPath $Path -Raw -Encoding utf8)
    foreach ($m in [regex]::Matches($text, '(?m)^#{1,6}\s+(.+?)\s*#*\s*$')) {
        $heading = $m.Groups[1].Value.Trim().ToLowerInvariant()
        $slug = [regex]::Replace($heading, '[^\p{L}\p{N}_\- ]', '').Replace(' ', '-')
        [void]$slugs.Add($slug)
    }
    return , $slugs
}

$markdown = @(
    (Join-Path $repoRoot 'AGENTS.md')
    (Join-Path $repoRoot 'README.md')
) + @(Get-ChildItem (Join-Path $repoRoot 'docs') -Recurse -Filter *.md | ForEach-Object { $_.FullName })

$slugCache = @{}
foreach ($file in $markdown) {
    $rel = Get-RelativePath $file
    $text = Remove-CodeFences (Get-Content -LiteralPath $file -Raw -Encoding utf8)
    foreach ($m in [regex]::Matches($text, '\]\(([^)\s]+)\)')) {
        $target = $m.Groups[1].Value
        if ($target -match '^(https?:|mailto:)') { continue }
        $pathPart, $anchor = $target.Split('#', 2)
        $resolved = if ($pathPart) {
            [System.IO.Path]::GetFullPath((Join-Path (Split-Path $file) ([uri]::UnescapeDataString($pathPart))))
        } else { $file }
        if (-not (Test-Path -LiteralPath $resolved)) {
            Add-Violation 'link' $rel "missing target: $target"
            continue
        }
        if ($anchor -and $resolved.EndsWith('.md')) {
            if (-not $slugCache.ContainsKey($resolved)) { $slugCache[$resolved] = Get-HeadingSlugs $resolved }
            if (-not $slugCache[$resolved].Contains([uri]::UnescapeDataString($anchor).ToLowerInvariant())) {
                Add-Violation 'anchor' $rel "no heading for #$anchor in $pathPart"
            }
        }
    }
}

$sourceExtensions = 'cpp|cc|c|h|hpp|qml|js|ps1|py|cmake|bat|iss|txt|json'
foreach ($file in $markdown) {
    $rel = Get-RelativePath $file
    $text = Remove-CodeFences (Get-Content -LiteralPath $file -Raw -Encoding utf8)
    foreach ($m in [regex]::Matches($text, "[\w./-]+\.(?:$sourceExtensions):\d+(?:-\d+)?\b")) {
        Add-Violation 'lines' $rel "source line number: $($m.Value) (cite the file and symbol instead)"
    }
}

function Test-QuotedPath([string]$Path) {
    if ($Path -match '[<>|?"]|\.\.\.|…') { return $true }
    $full = Join-Path $repoRoot $Path
    if ($Path -match '^(.*\.)h/cpp$') {
        $stem = Join-Path $repoRoot $Matches[1]
        return (Test-Path -LiteralPath ($stem + 'h')) -or (Test-Path -LiteralPath ($stem + 'cpp'))
    }
    if ($Path.Contains('*')) {
        return @(Get-ChildItem -Path $full -ErrorAction SilentlyContinue).Count -gt 0
    }
    return Test-Path -LiteralPath $full
}

foreach ($file in $markdown) {
    $rel = Get-RelativePath $file
    $text = Get-Content -LiteralPath $file -Raw -Encoding utf8
    foreach ($m in [regex]::Matches($text, '`((?:src|qml|tests|tools)/[^`\s]+)`')) {
        $path = $m.Groups[1].Value.TrimEnd('.', ',', ';', ':')
        if (-not (Test-QuotedPath $path)) {
            Add-Violation 'path' $rel "no such file or directory: $path"
        }
    }
}

$codeRoots = @('src', 'qml', 'tests', 'tools') | ForEach-Object { Join-Path $repoRoot $_ }
$codeFiles = @(Get-ChildItem $codeRoots -Recurse -File -Include *.cpp, *.h, *.qml, *.ps1, *.cmake, *.txt, *.py -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\vendor\\' })
$codeFiles += Get-Item (Join-Path $repoRoot 'CMakeLists.txt')
foreach ($file in $codeFiles) {
    if ($file.FullName -eq $PSCommandPath) { continue }
    # Whole-file match: reading line by line made this the slowest part of the check.
    $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
    foreach ($m in [regex]::Matches($text, '\bdocs/[\w./-]+\.md')) {
        if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $m.Value))) {
            $lineNo = $text.Substring(0, $m.Index).Split("`n").Count
            Add-Violation 'backref' ("{0}:{1}" -f (Get-RelativePath $file.FullName), $lineNo) "missing doc: $($m.Value)"
        }
    }
}

$indexPath = Join-Path $repoRoot 'docs/index.md'
$indexText = Remove-CodeFences (Get-Content -LiteralPath $indexPath -Raw -Encoding utf8)
$indexed = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($m in [regex]::Matches($indexText, '\]\(([^)#\s]+)')) {
    [void]$indexed.Add([System.IO.Path]::GetFullPath((Join-Path (Split-Path $indexPath) $m.Groups[1].Value)))
}
foreach ($doc in (Get-ChildItem (Join-Path $repoRoot 'docs') -Recurse -Filter *.md)) {
    $rel = Get-RelativePath $doc.FullName
    if ($rel -eq 'docs/index.md') { continue }
    if (-not $indexed.Contains($doc.FullName)) {
        Add-Violation 'index' 'docs/index.md' "not linked: $rel"
    }
}

$vendorBinaries = @(
    @{ Doc = 'docs/legal/libghostty-vt.md'; File = 'src/terminal/vendor/libghostty-vt/lib/libghostty-vt.a' }
    @{ Doc = 'docs/legal/conpty.md'; File = 'src/terminal/vendor/conpty/x64/conpty.dll' }
    @{ Doc = 'docs/legal/conpty.md'; File = 'src/terminal/vendor/conpty/x64/OpenConsole.exe' }
)
foreach ($binary in $vendorBinaries) {
    $binaryPath = Join-Path $repoRoot $binary.File
    if (-not (Test-Path -LiteralPath $binaryPath)) {
        Add-Violation 'vendor' $binary.Doc "missing binary: $($binary.File)"
        continue
    }
    $recorded = Get-Content -LiteralPath (Join-Path $repoRoot $binary.Doc) -Raw -Encoding utf8
    $hash = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash
    if ($recorded.IndexOf($hash, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        Add-Violation 'vendor' $binary.Doc "does not record the SHA-256 of $($binary.File) ($hash)"
    }
}

# Ownership map consistency lives in check-ownership.ps1; it runs as part of this check.
& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'check-ownership.ps1') -Quiet
$ownershipFailed = $LASTEXITCODE -ne 0

if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Output $_ }
    Write-Output "`nDocs check FAILED: $($violations.Count) violation(s)."
    exit 1
}
if ($ownershipFailed) {
    Write-Output "`nDocs check FAILED: ownership map (see above)."
    exit 1
}
Write-Output 'Docs check passed.'
exit 0
