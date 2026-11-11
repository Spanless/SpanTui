# tools/fonts/build-fonts.ps1 — rebuild the bundled Span Mono / Span Sans /
# Span Symbols fonts.
#
# Pipeline (docs/services/application/settings-appearance.md, 内置字体):
#   1. download the pinned Geist, Figtree and Nerd Fonts symbols-only
#      releases (all SHA-256 checked);
#   2. tools/fonts/make_span_fonts.py renames every Geist Mono / Figtree
#      weight, freezes the chosen stylistic sets, draws Span's Powerline
#      separators and replacement character (U+FFFD) into Span Mono and
#      builds Span Symbols from the Nerd icons;
#   3. copy the faces and licenses into assets/fonts/.
#
# Needs Python 3 with fontTools. Work files live in .dev/tmp/fontbuild/ (ignored).
# The faces in assets/fonts/ are committed, so a normal build never runs this.
#
# Usage:  tools\fonts\build-fonts.ps1

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$GeistVersion = 'v1.7.2'
$GeistSha256 = '7fc800d2ac6b92844895196e5041aca55d814c15db70c44f79b3b83ab82b04e2'
$FigtreeVersion = '2.0.3'
$FigtreeSha256 = 'bfa90c762a2d0a5d6cbeb549f21cb18c9b36047439805a0ad029314bc66a3351'
$NerdFontsVersion = 'v3.5.1'
$NerdSymbolsSha256 = 'fdca3682534f6f65e1ccb2345b0362ccf67d9b8eca7c8025330946e93e2473bc'

$repoRoot = Split-Path (Split-Path $PSScriptRoot)
$workDir = Join-Path $repoRoot '.dev/tmp/fontbuild'
$stageDir = Join-Path $workDir 'out'
$assetsDir = Join-Path $repoRoot 'assets/fonts'

function Get-PinnedArchive([string]$Url, [string]$Sha256, [string]$Destination) {
    $zip = Join-Path $workDir (Split-Path $Url -Leaf)
    if (-not (Test-Path $zip)) {
        Invoke-WebRequest -Uri $Url -OutFile $zip
    }
    $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) {
        Remove-Item $zip
        throw "$(Split-Path $Url -Leaf) SHA-256 mismatch: $actual"
    }
    if (Test-Path $Destination) {
        Remove-Item -Recurse -Force $Destination
    }
    Expand-Archive -Path $zip -DestinationPath $Destination
}

New-Item -ItemType Directory -Force $workDir | Out-Null
$geistDir = Join-Path $workDir 'geist'
$figtreeDir = Join-Path $workDir 'figtree'
$symbolsDir = Join-Path $workDir 'nf-symbols'
Get-PinnedArchive "https://github.com/vercel/geist-font/releases/download/$GeistVersion/geist-font-$GeistVersion.zip" `
    $GeistSha256 $geistDir
Get-PinnedArchive "https://github.com/erikdkennedy/figtree/archive/refs/tags/v$FigtreeVersion.zip" `
    $FigtreeSha256 $figtreeDir
Get-PinnedArchive "https://github.com/ryanoasis/nerd-fonts/releases/download/$NerdFontsVersion/NerdFontsSymbolsOnly.zip" `
    $NerdSymbolsSha256 $symbolsDir

if (Test-Path $stageDir) {
    Remove-Item -Recurse -Force $stageDir
}
python (Join-Path $PSScriptRoot 'make_span_fonts.py') `
    --geist (Join-Path $geistDir 'geist-font') `
    --figtree (Join-Path $figtreeDir "figtree-$FigtreeVersion") `
    --symbols (Join-Path $symbolsDir 'SymbolsNerdFontMono-Regular.ttf') `
    --output $stageDir
if ($LASTEXITCODE -ne 0) {
    throw "make_span_fonts.py failed with exit code $LASTEXITCODE"
}

Get-ChildItem $assetsDir -Filter 'Span*.ttf' | Remove-Item
Copy-Item (Join-Path $stageDir '*.ttf') $assetsDir
Get-ChildItem $assetsDir -Filter 'SpanFonts-LICENSE-*.txt' | Remove-Item
Copy-Item (Join-Path $geistDir 'geist-font/OFL.txt') (Join-Path $assetsDir 'SpanFonts-LICENSE-Geist.txt')
Copy-Item (Join-Path $figtreeDir "figtree-$FigtreeVersion/OFL.txt") (Join-Path $assetsDir 'SpanFonts-LICENSE-Figtree.txt')
Copy-Item (Join-Path $symbolsDir 'LICENSE') (Join-Path $assetsDir 'SpanFonts-LICENSE-NerdFonts.txt')
Write-Host "Span fonts written to $assetsDir"
