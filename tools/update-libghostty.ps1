# Rebuild the vendored libghostty-vt static
# library (x86_64-windows-gnu, linked by MinGW) from an upstream Ghostty commit.
#
# Clones Ghostty into a fresh directory under .dev\tmp\ (or uses -Source, an
# existing checkout at that commit), builds it with Zig, strips debug info and
# the `.drectve` linker directives GNU ld does not understand, repacks the
# archive under bare member names, and replaces
# src/terminal/vendor/libghostty-vt/{include,lib}. Prints the hash to record
# in docs/legal/libghostty-vt.md. Needs Zig (minimum version in Ghostty's
# build.zig.zon) and LLVM's llvm-ar/llvm-strip/llvm-objcopy; network access
# unless -Source and Zig's package cache already hold everything. Run it only
# when upgrading.
#
# Usage:  tools\update-libghostty.ps1 -Commit <sha> -Zig <path\to\zig.exe>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Commit,
    [Parameter(Mandatory = $true)][string]$Zig,
    [string]$Source,
    [string]$LlvmBin = 'C:\Program Files\LLVM\bin'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
$target = Join-Path $repoRoot 'src\terminal\vendor\libghostty-vt'
$work = Join-Path $repoRoot ('.dev\tmp\libghostty-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
foreach ($tool in 'llvm-ar.exe', 'llvm-strip.exe', 'llvm-objcopy.exe') {
    if (-not (Test-Path -LiteralPath (Join-Path $LlvmBin $tool))) { throw "$tool not found in $LlvmBin." }
}
try {
    if (-not $Source) {
        $Source = Join-Path $work 'ghostty'
        git clone --quiet --filter=blob:none https://github.com/ghostty-org/ghostty $Source
        if ($LASTEXITCODE -ne 0) { throw 'git clone failed.' }
    }
    git -C $Source -c advice.detachedHead=false checkout --quiet $Commit
    if ($LASTEXITCODE -ne 0) { throw "Cannot check out $Commit." }
    $head = (git -C $Source rev-parse HEAD).Trim()

    # Zig's caches stay inside the repository's ignored .dev\ tree.
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $repoRoot '.dev\tools\zig-cache\global'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $repoRoot '.dev\tools\zig-cache\local'
    $out = Join-Path $work 'out'
    Push-Location $Source
    try {
        & $Zig build -Demit-lib-vt -Dtarget=x86_64-windows-gnu -Doptimize=ReleaseFast --prefix $out
        if ($LASTEXITCODE -ne 0) { throw 'zig build failed.' }
    } finally {
        Pop-Location
    }

    $library = Join-Path $out 'lib\ghostty-vt-static.lib'
    & (Join-Path $LlvmBin 'llvm-strip.exe') --strip-debug $library
    if ($LASTEXITCODE -ne 0) { throw 'llvm-strip failed.' }
    & (Join-Path $LlvmBin 'llvm-objcopy.exe') --remove-section=.drectve $library
    if ($LASTEXITCODE -ne 0) { throw 'llvm-objcopy failed.' }

    # Zig names each archive member after its object file in the build cache,
    # an absolute path of this machine. Repack under the bare file names,
    # keeping the member order, so the committed binary carries no local path.
    $llvmAr = Join-Path $LlvmBin 'llvm-ar.exe'
    $memberNames = @(& $llvmAr t $library | ForEach-Object { Split-Path $_ -Leaf })
    if ($LASTEXITCODE -ne 0) { throw 'llvm-ar t failed.' }
    if (@($memberNames | Select-Object -Unique).Count -ne $memberNames.Count) {
        throw 'Archive member names are not unique once their directories are dropped.'
    }
    $memberDir = Join-Path $work 'members'
    New-Item -ItemType Directory -Force -Path $memberDir | Out-Null
    $repacked = Join-Path $work 'libghostty-vt.a'
    Push-Location $memberDir
    try {
        & $llvmAr x $library
        if ($LASTEXITCODE -ne 0) { throw 'llvm-ar x failed.' }
        & $llvmAr rcsD $repacked @memberNames
        if ($LASTEXITCODE -ne 0) { throw 'llvm-ar rcsD failed.' }
    } finally {
        Pop-Location
    }
    $library = $repacked

    foreach ($part in 'include', 'lib') {
        Remove-Item -LiteralPath (Join-Path $target $part) -Recurse -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $target 'lib') | Out-Null
    Copy-Item -LiteralPath (Join-Path $out 'include') -Destination $target -Recurse
    Copy-Item -LiteralPath $library -Destination (Join-Path $target 'lib\libghostty-vt.a')
    Copy-Item -LiteralPath (Join-Path $Source 'LICENSE') -Destination (Join-Path $target 'LICENSE') -Force

    Write-Host ('Commit {0}' -f $head)
    Write-Host ('libghostty-vt.a  sha256 {0}' -f
        (Get-FileHash -LiteralPath (Join-Path $target 'lib\libghostty-vt.a') -Algorithm SHA256).Hash)
    Write-Host ('Zig {0}' -f (& $Zig version))
    Write-Host 'Record the commit, Zig version and hash in docs/legal/libghostty-vt.md.'
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
