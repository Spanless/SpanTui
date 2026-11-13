# Shared Qt/MinGW/CMake toolchain resolution for the devtool workers and start.bat.
# Priority: process environment > this checkout's .env > the main checkout's
# .env (linked worktrees only) > discovery under the Qt install root
# (SPANTUI_QT_SDK_ROOT, else <drive>:\Qt, else the root of the Qt tools on
# PATH) > PATH discovery.

Set-StrictMode -Version Latest

$script:SpanTuiRepoRoot = Split-Path -Parent $PSScriptRoot

# The repository's sccache server listens on a port of its own in this range,
# picked from the main checkout's path, so it never meets the server other
# projects start on sccache's default port.
$script:SpanTuiSccachePortBase = 42300
$script:SpanTuiSccachePortCount = 500

# A linked worktree has a .git file naming <main>\.git\worktrees\<id>, whose
# `commondir` leads to the main .git. Name: `main` or `wt-<id>`. Anything else
# (no .git, a submodule) is a main checkout of its own.
function Get-SpanTuiCheckout {
    param([string]$Root = $script:SpanTuiRepoRoot)
    $checkoutRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    $dotGit = Join-Path $checkoutRoot '.git'
    if (Test-Path -LiteralPath $dotGit -PathType Leaf) {
        $pointer = @([System.IO.File]::ReadAllLines($dotGit) | Where-Object { $_ -match '^gitdir:\s*\S' }) | Select-Object -First 1
        if ($pointer) {
            $gitDir = Resolve-SpanTuiRepoPath $checkoutRoot ($pointer -replace '^gitdir:\s*', '').Trim()
            $commonFile = Join-Path $gitDir 'commondir'
            if (Test-Path -LiteralPath $commonFile -PathType Leaf) {
                $commonDir = Resolve-SpanTuiRepoPath $gitDir ([System.IO.File]::ReadAllText($commonFile).Trim())
                return [pscustomobject]@{
                    Root = $checkoutRoot
                    MainRoot = Split-Path -Parent $commonDir
                    Name = 'wt-' + (Split-Path -Leaf $gitDir)
                    IsWorktree = $true
                }
            }
        }
    }
    return [pscustomobject]@{ Root = $checkoutRoot; MainRoot = $checkoutRoot; Name = 'main'; IsWorktree = $false }
}

# Linked worktrees of a main checkout, read from its .git\worktrees admin
# folders without starting git: Id, Name (wt-<id>), Path, Exists.
function Get-SpanTuiLinkedCheckouts {
    param([Parameter(Mandatory = $true)][string]$MainRoot)
    $adminRoot = Join-Path $MainRoot '.git\worktrees'
    $checkouts = @()
    if (-not (Test-Path -LiteralPath $adminRoot -PathType Container)) { return $checkouts }
    foreach ($admin in Get-ChildItem -LiteralPath $adminRoot -Directory) {
        $gitdirFile = Join-Path $admin.FullName 'gitdir'
        if (-not (Test-Path -LiteralPath $gitdirFile -PathType Leaf)) { continue }
        $dotGit = Resolve-SpanTuiRepoPath $admin.FullName ([System.IO.File]::ReadAllText($gitdirFile).Trim())
        $path = Split-Path -Parent $dotGit
        $checkouts += [pscustomobject]@{
            Id = $admin.Name
            Name = "wt-$($admin.Name)"
            Path = $path
            Exists = Test-Path -LiteralPath $path -PathType Container
        }
    }
    return $checkouts
}

# First 16 hex digits of SHA-256 over the lower-cased full path.
function Get-SpanTuiPathKey([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString(
            $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(
                [System.IO.Path]::GetFullPath($Path).TrimEnd('\').ToLowerInvariant()))).Replace('-', '').Substring(0, 16)
    } finally {
        $sha.Dispose()
    }
}

# Commands of one checkout share its build tree, so dev.ps1 runs them one at a
# time under this mutex; `dev worktree remove` takes it to keep a worktree
# that is building or testing.
function New-SpanTuiDevMutex {
    param([Parameter(Mandatory = $true)][string]$Root)
    return New-Object System.Threading.Mutex($false, "Local\SpanTui.Dev.$(Get-SpanTuiPathKey $Root)")
}

# True while a dev command holds the checkout's mutex. Never call it for the
# checkout this process runs in: dev.ps1 holds that mutex itself.
function Test-SpanTuiCheckoutBusy([string]$Root) {
    $mutex = New-SpanTuiDevMutex -Root $Root
    try {
        try { $free = $mutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $free = $true }
        if ($free) { $mutex.ReleaseMutex() }
        return -not $free
    } finally {
        $mutex.Dispose()
    }
}

# KEY=VALUE, optionally quoted. A variable already set in the process wins. A
# worktree reads its own .env, then the main checkout's.
function Read-SpanTuiDotEnv {
    param([Parameter(Mandatory = $true)][string]$Path)
    $values = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $values }
    $number = 0
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        ++$number
        $text = $line.Trim()
        if ($text.Length -eq 0 -or $text.StartsWith('#')) { continue }
        if ($text -notmatch '^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$') {
            Fail ("{0}:{1}: expected KEY=VALUE" -f $Path, $number)
        }
        $name, $value = $Matches[1], $Matches[2].Trim()
        if ($value.Length -ge 2 -and $value[0] -eq $value[-1] -and $value[0] -in @('"', "'")) {
            $value = $value.Substring(1, $value.Length - 2)
        }
        $values[$name] = $value
    }
    return $values
}

function Import-SpanTuiDotEnv {
    param([string]$Root = $script:SpanTuiRepoRoot)
    $checkout = Get-SpanTuiCheckout -Root $Root
    $files = @(Join-Path $checkout.Root '.env')
    if ($checkout.IsWorktree) { $files += Join-Path $checkout.MainRoot '.env' }
    foreach ($file in $files) {
        $values = Read-SpanTuiDotEnv -Path $file
        foreach ($name in $values.Keys) {
            if ([string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name))) {
                [Environment]::SetEnvironmentVariable($name, $values[$name])
            }
        }
    }
}

# Layout of .dev/: docs/reference/paths-configuration.md.
function Get-SpanTuiDevPath {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$Root = $script:SpanTuiRepoRoot
    )
    $path = Join-Path (Join-Path $Root '.dev') $Name
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    return $path
}

# Data root argument for every development launch of the product.
function Get-SpanTuiDevDataArgs {
    param([string]$Root = $script:SpanTuiRepoRoot)
    return @('--data-dir', (Get-SpanTuiDevPath -Name 'data' -Root $Root))
}

function Write-SpanError([string]$Message) {
    Write-Host "ERROR: $Message" -ForegroundColor Red
}

function Fail([string]$Message) {
    Write-SpanError $Message
    # Throw (instead of exit) so callers' try/finally blocks can clean staging dirs.
    throw $Message
}

function Require-File([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "$Label was not found: $Path"
    }
}

# First candidate that exists (blank and null candidates are skipped).
function Get-FirstExistingPath {
    param(
        [AllowNull()][AllowEmptyCollection()][string[]]$Candidates = @(),
        [switch]$Directory
    )
    $type = if ($Directory) { 'Container' } else { 'Leaf' }
    foreach ($candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType $type)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Get-Sha256([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

# A Ninja tree is reused while it was configured with the same arguments
# (options and toolchain, recorded in .spantui-configure). A missing
# build.ninja or another generator (an IDE reusing the tree) means configure.
function Test-SpanTuiBuildTreeCurrent {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $stamp = Join-Path $BuildDir '.spantui-configure'
    return (Test-Path -LiteralPath (Join-Path $BuildDir 'build.ninja') -PathType Leaf) -and
        (Test-SpanTuiNinjaCache $BuildDir) -and
        (Test-Path -LiteralPath $stamp -PathType Leaf) -and
        ([System.IO.File]::ReadAllText($stamp) -ceq ($Arguments -join "`n"))
}

function Test-SpanTuiNinjaCache([string]$BuildDir) {
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    return (Test-Path -LiteralPath $cache -PathType Leaf) -and
        [bool](Select-String -LiteralPath $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=Ninja$' -Quiet)
}

# Changing Qt6_DIR alone keeps the old kit's Qt6<Module>_DIR cache entries, so
# one build would mix two Qt versions. True when the cache resolved Qt6Core
# outside QtRoot.
function Test-SpanTuiCacheForeignQt([string]$BuildDir, [string]$QtRoot) {
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) { return $false }
    $entry = Select-String -LiteralPath $cache -Pattern '^Qt6Core_DIR:[A-Z]+=(.+)$' | Select-Object -First 1
    if (-not $entry) { return $false }
    $normalize = { param($path) ($path.Trim() -replace '\\', '/').TrimEnd('/') + '/' }
    return -not (& $normalize $entry.Matches[0].Groups[1].Value).StartsWith(
        (& $normalize $QtRoot), [System.StringComparison]::OrdinalIgnoreCase)
}

# CMake refuses a cache made for another source folder ("does not match the
# source used to generate cache"), as after moving the checkout. True when the
# cache names a source folder other than SourceDir.
function Test-SpanTuiCacheForeignSource([string]$BuildDir, [string]$SourceDir) {
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) { return $false }
    $entry = Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$' | Select-Object -First 1
    if (-not $entry) { return $false }
    $normalize = { param($path) ($path.Trim() -replace '\\', '/').TrimEnd('/') }
    return -not [string]::Equals((& $normalize $entry.Matches[0].Groups[1].Value), (& $normalize $SourceDir),
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Invoke-SpanTuiTimedStep {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )
    Write-Host ">> $Name..."
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        & $Action
    } finally {
        $sw.Stop()
        Write-Host ("   {0} done in {1:n1}s" -f $Name, $sw.Elapsed.TotalSeconds) -ForegroundColor DarkGray
    }
}

function Get-SpanTuiVersion([string]$CMakeListsPath) {
    Require-File $CMakeListsPath 'CMake project file'
    $versionMatch = Select-String -LiteralPath $CMakeListsPath -Pattern '^project\(SpanTui\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' |
        Select-Object -First 1
    if (-not $versionMatch) {
        Fail 'Could not read the SpanTui version from CMakeLists.txt.'
    }
    return $versionMatch.Matches[0].Groups[1].Value
}

# Build trees and packages live under the git-ignored target\ of each
# checkout. Optional variables (environment or .env) send them elsewhere; a
# relative value is taken from the main checkout, so one .env line means the
# same place for every worktree:
#   SPANTUI_BUILD_ROOT     each checkout builds in <root>\main or <root>\wt-<id>
#                          (default <checkout>\target\build)
#   SPANTUI_ARTIFACT_ROOT  packages go to <root>\span-tui\<version>\windows-x64
#                          (default <checkout>\target\release\<version>\windows-x64;
#                          the folder above the versions is the app's local update source)
#   SPANTUI_WORKTREE_ROOT  where `dev worktree add` creates worktrees
#                          (default <main checkout>.worktrees beside it)
function Resolve-SpanTuiRepoPath([string]$Root, [string]$Path) {
    return [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($Root, $Path)).TrimEnd('\')
}

function Resolve-SpanTuiBuildDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string]$BuildRoot = $env:SPANTUI_BUILD_ROOT,
        [string]$RetiredBuildDir = $env:SPANTUI_BUILD_DIR
    )
    $checkout = Get-SpanTuiCheckout -Root $Root
    if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
        # One shared tree for every checkout was the old meaning; building in
        # target\ instead would silently fill the checkout.
        if (-not [string]::IsNullOrWhiteSpace($RetiredBuildDir)) {
            Fail 'SPANTUI_BUILD_DIR is no longer read. Set SPANTUI_BUILD_ROOT instead (each checkout builds in <root>\main or <root>\wt-<id>), or remove it to build in target\build.'
        }
        return Resolve-SpanTuiRepoPath $checkout.Root 'target\build'
    }
    return Join-Path (Resolve-SpanTuiRepoPath $checkout.MainRoot $BuildRoot) $checkout.Name
}

function Resolve-SpanTuiReleaseDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Version,
        [string]$ArtifactRoot = $env:SPANTUI_ARTIFACT_ROOT
    )
    $checkout = Get-SpanTuiCheckout -Root $Root
    $slot = "$Version\windows-x64"
    if ([string]::IsNullOrWhiteSpace($ArtifactRoot)) { return Resolve-SpanTuiRepoPath $checkout.Root "target\release\$slot" }
    return Resolve-SpanTuiRepoPath $checkout.MainRoot (Join-Path $ArtifactRoot "span-tui\$slot")
}

function Resolve-SpanTuiWorktreeRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string]$WorktreeRoot = $env:SPANTUI_WORKTREE_ROOT
    )
    $main = (Get-SpanTuiCheckout -Root $Root).MainRoot
    if ([string]::IsNullOrWhiteSpace($WorktreeRoot)) { return "$main.worktrees" }
    return Resolve-SpanTuiRepoPath $main $WorktreeRoot
}

# Newest <version>\mingw_64 kit under a Qt install root, or $null.
function Find-SpanTuiQtKit([string]$SdkRoot) {
    if (-not $SdkRoot -or -not (Test-Path -LiteralPath $SdkRoot -PathType Container)) { return $null }
    return Get-ChildItem -LiteralPath $SdkRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+(\.\d+){1,2}$' } |
        Sort-Object { [version]$_.Name } -Descending | ForEach-Object { Join-Path $_.FullName 'mingw_64' } |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'bin\qmake.exe') } | Select-Object -First 1
}

# The Qt install root (the folder holding <version>\ and Tools\). A set
# SPANTUI_QT_SDK_ROOT must exist. Otherwise the first root that holds a
# mingw_64 kit among <drive>:\Qt on the fixed drives (the installer's default
# is C:\Qt) and the roots of qmake.exe, qtpaths.exe (<root>\<version>\mingw_64\bin)
# and cmake.exe (<root>\Tools\CMake_64\bin) on PATH; $null when none does.
function Resolve-SpanTuiQtSdkRoot {
    if (-not [string]::IsNullOrWhiteSpace($env:SPANTUI_QT_SDK_ROOT)) {
        if (-not (Test-Path -LiteralPath $env:SPANTUI_QT_SDK_ROOT -PathType Container)) {
            Fail "SPANTUI_QT_SDK_ROOT=$env:SPANTUI_QT_SDK_ROOT does not exist; point it at the Qt install root (the folder holding 6.x.y\ and Tools\). See docs/workflow/build.md."
        }
        return (Resolve-Path -LiteralPath $env:SPANTUI_QT_SDK_ROOT).Path
    }
    $candidates = @([System.IO.DriveInfo]::GetDrives() |
        Where-Object { $_.DriveType -eq [System.IO.DriveType]::Fixed -and $_.IsReady } |
        ForEach-Object { Join-Path $_.RootDirectory.FullName 'Qt' })
    foreach ($tool in @('qmake.exe', 'qtpaths.exe', 'cmake.exe')) {
        foreach ($command in @(Get-Command $tool -CommandType Application -All -ErrorAction SilentlyContinue)) {
            $up3 = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $command.Source))
            if ($tool -eq 'cmake.exe') {
                if ((Split-Path -Leaf $up3) -ieq 'Tools') { $candidates += Split-Path -Parent $up3 }
            } else {
                $candidates += Split-Path -Parent $up3
            }
        }
    }
    foreach ($candidate in $candidates) {
        if (Find-SpanTuiQtKit $candidate) { return [System.IO.Path]::GetFullPath($candidate).TrimEnd('\') }
    }
    return $null
}

# Qt, MinGW, CMake and Ninja. Each SPANTUI_* variable (environment or .env)
# sets one piece; the rest is discovered under the Qt install root
# (Resolve-SpanTuiQtSdkRoot: newest <version>\mingw_64 kit, Tools\mingw*\bin,
# Tools\CMake_64, Tools\Ninja), then on PATH. A miss names the variable that
# fixes it.
function Resolve-SpanTuiToolchain {
    param([switch]$RequireWindeployQt)

    $sdk = Resolve-SpanTuiQtSdkRoot
    $sdkPath = { param($relative) if ($sdk) { Join-Path $sdk $relative } }
    $onPath = { param($name) $command = Get-Command $name -ErrorAction SilentlyContinue; if ($command) { $command.Source } }

    $qtRoot = $env:SPANTUI_QT_ROOT
    if (-not $qtRoot) { $qtRoot = Find-SpanTuiQtKit $sdk }
    $mingwBin = $env:SPANTUI_MINGW_BIN
    if (-not $mingwBin -and $sdk) {
        $mingwBin = Get-ChildItem -LiteralPath (& $sdkPath 'Tools') -Directory -Filter 'mingw*' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'bin' } |
            Where-Object { Test-Path -LiteralPath (Join-Path $_ 'g++.exe') } | Select-Object -First 1
    }
    $cmake = $env:SPANTUI_CMAKE
    if (-not $cmake) { $cmake = Get-FirstExistingPath @((& $sdkPath 'Tools\CMake_64\bin\cmake.exe'), (& $onPath 'cmake.exe')) }
    $ninjaDir = $env:SPANTUI_NINJA_DIR
    if (-not $ninjaDir) {
        $ninja = Get-FirstExistingPath @((& $sdkPath 'Tools\Ninja\ninja.exe'), (& $onPath 'ninja.exe'))
        if ($ninja) { $ninjaDir = Split-Path -Parent $ninja }
    }

    $missing = @()
    if (-not $qtRoot) { $missing += 'Qt mingw_64 kit (no <drive>:\Qt and no Qt tools on PATH: set SPANTUI_QT_SDK_ROOT to the Qt install root, or SPANTUI_QT_ROOT to the kit)' }
    if (-not $cmake) { $missing += 'CMake (SPANTUI_CMAKE, or cmake.exe on PATH)' }
    if (-not $mingwBin) { $missing += 'MinGW bin directory (SPANTUI_MINGW_BIN)' }
    if (-not $ninjaDir) { $missing += 'Ninja directory (SPANTUI_NINJA_DIR, or ninja.exe on PATH)' }
    if ($missing.Count -gt 0) {
        Fail ('Toolchain not found: ' + ($missing -join '; ') + '. Put the variables in .env (template: .env.example); dev.bat doctor shows what was found. See docs/workflow/build.md.')
    }

    $qtRoot = [System.IO.Path]::GetFullPath($qtRoot)
    $cmake = [System.IO.Path]::GetFullPath($cmake)
    $mingwBin = [System.IO.Path]::GetFullPath($mingwBin)
    $ninjaDir = [System.IO.Path]::GetFullPath($ninjaDir)
    $toolchain = [pscustomobject]@{
        QtRoot      = $qtRoot
        Qt6Dir      = Join-Path $qtRoot 'lib\cmake\Qt6'
        CMake       = $cmake
        CTest       = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
        MingwBin    = $mingwBin
        NinjaDir    = $ninjaDir
        Ninja       = Join-Path $ninjaDir 'ninja.exe'
        Compiler    = Join-Path $mingwBin 'g++.exe'
        CCompiler   = Join-Path $mingwBin 'gcc.exe'
        WindeployQt = Join-Path $qtRoot 'bin\windeployqt.exe'
    }
    Require-File (Join-Path $qtRoot 'bin\qmake.exe') 'Qt SDK (qmake)'
    Require-File (Join-Path $toolchain.Qt6Dir 'Qt6Config.cmake') 'Qt6 CMake package'
    Require-File $toolchain.CMake 'CMake'
    Require-File $toolchain.CTest 'CTest'
    Require-File $toolchain.Ninja 'Ninja'
    Require-File $toolchain.Compiler 'MinGW g++'
    Require-File $toolchain.CCompiler 'MinGW gcc'
    if ($RequireWindeployQt) { Require-File $toolchain.WindeployQt 'windeployqt' }
    $toolchain | Add-Member -NotePropertyName LinkerDir -NotePropertyValue (Resolve-SpanTuiLinkerDirectory)
    return $toolchain
}

# g++ -fuse-ld looks ld.<name>.exe up on PATH; Use-SpanTuiToolchainPath puts
# this folder first, so it is the one g++ uses.
function Resolve-SpanTuiLinkerDirectory {
    param(
        [string]$Linker = $env:SPANTUI_LINKER,
        [string]$LinkerDir = $env:SPANTUI_LINKER_DIR
    )
    if ([string]::IsNullOrWhiteSpace($Linker)) { return '' }
    $exe = "ld.$Linker.exe"
    $candidates = @()
    if ($LinkerDir) {
        $candidates += $LinkerDir
    } else {
        $command = Get-Command $exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { $candidates += Split-Path -Parent $command.Source }
        if ($Linker -eq 'lld' -and $env:ProgramFiles) { $candidates += Join-Path $env:ProgramFiles 'LLVM\bin' }
    }
    foreach ($dir in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $dir $exe) -PathType Leaf) { return [System.IO.Path]::GetFullPath($dir) }
    }
    Fail "SPANTUI_LINKER=$Linker, but $exe was not found: install it (lld comes with LLVM) or set SPANTUI_LINKER_DIR to its folder. See docs/workflow/build.md."
}

function Use-SpanTuiToolchainPath([pscustomobject]$Toolchain) {
    $env:PATH = "$(Join-Path $Toolchain.QtRoot 'bin');$($Toolchain.MingwBin);$($Toolchain.NinjaDir);$env:PATH"
    if ($Toolchain.LinkerDir) { $env:PATH = "$($Toolchain.LinkerDir);$env:PATH" }
    Use-SpanTuiCompilerCache
}

# Compiler cache (docs/workflow/build.md, section 1). CMakeLists.txt compiles
# through sccache when it is on PATH. All checkouts of the repository share
# one sccache server of their own: a port derived from the main checkout, a
# cache folder (SPANTUI_SCCACHE_DIR, default <SPANTUI_BUILD_ROOT>\sccache or
# <main checkout>\target\sccache), and every checkout root and build tree as
# base directories, which sccache strips from paths before hashing, so one
# checkout reuses what another compiled. SPANTUI_SCCACHE=external leaves
# sccache's environment alone (CI starts its own server).
function Get-SpanTuiCompilerCacheConfig {
    param([string]$Root = $script:SpanTuiRepoRoot)
    $main = (Get-SpanTuiCheckout -Root $Root).MainRoot
    $checkouts = @($main) + @(Get-SpanTuiLinkedCheckouts -MainRoot $main | Where-Object { $_.Exists } | ForEach-Object { $_.Path })
    $baseDirs = @()
    foreach ($checkout in $checkouts) {
        $baseDirs += $checkout
        $build = Resolve-SpanTuiBuildDirectory -Root $checkout
        if (-not "$build\".StartsWith("$checkout\", [System.StringComparison]::OrdinalIgnoreCase)) { $baseDirs += $build }
    }
    $directory = $env:SPANTUI_SCCACHE_DIR
    if ([string]::IsNullOrWhiteSpace($directory)) {
        $directory = if ([string]::IsNullOrWhiteSpace($env:SPANTUI_BUILD_ROOT)) { 'target\sccache' } else { Join-Path $env:SPANTUI_BUILD_ROOT 'sccache' }
    }
    $port = $script:SpanTuiSccachePortBase + [Convert]::ToInt32((Get-SpanTuiPathKey $main).Substring(0, 6), 16) % $script:SpanTuiSccachePortCount
    return [pscustomobject]@{
        MainRoot = $main
        Checkouts = $checkouts
        Port = $port
        Directory = Resolve-SpanTuiRepoPath $main $directory
        # Longest first, so a tree nested in another root is matched as itself.
        BaseDirs = @($baseDirs | Sort-Object -Unique | Sort-Object -Property Length -Descending)
    }
}

# sccache spells paths lower-case with forward slashes and a trailing slash.
function ConvertTo-SpanTuiSccachePath([string]$Path) {
    return ($Path.Trim() -replace '\\', '/').TrimEnd('/').ToLowerInvariant() + '/'
}

# Points this process (and the compiles it starts) at the repository's sccache
# server and makes sure the server knows every checkout. The server reads its
# base directories once, at start: when the checkouts changed it is restarted,
# unless a dev command in another checkout may be compiling through it, which
# a restart would break; that build then runs without the new checkouts.
function Use-SpanTuiCompilerCache {
    param([string]$Root = $script:SpanTuiRepoRoot)
    switch ("$env:SPANTUI_SCCACHE") {
        '' { }
        'external' { return }
        default { Fail "SPANTUI_SCCACHE=$env:SPANTUI_SCCACHE is not a known value: leave it unset, or set external to keep sccache's own environment. See docs/workflow/build.md." }
    }
    $sccache = Get-Command sccache.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $sccache) { return }
    $config = Get-SpanTuiCompilerCacheConfig -Root $Root
    New-Item -ItemType Directory -Force -Path $config.Directory | Out-Null
    $env:SCCACHE_SERVER_PORT = "$($config.Port)"
    $env:SCCACHE_DIR = $config.Directory
    $env:SCCACHE_BASEDIRS = $config.BaseDirs -join ';'

    # Starts the server with the variables above when none listens yet.
    $server = Get-SpanTuiCompilerCacheServer $sccache.Source
    if (-not $server) {
        Write-Host 'Compiler cache: sccache did not answer; compiling through it anyway.' -ForegroundColor Yellow
        return
    }
    if ($null -eq $server.BaseDirs) {
        Write-Host "Compiler cache: sccache $($server.Version) has no base directories; checkouts do not share objects until it is updated." -ForegroundColor Yellow
        return
    }
    $wanted = @($config.BaseDirs | ForEach-Object { ConvertTo-SpanTuiSccachePath $_ } | Sort-Object)
    $actual = @($server.BaseDirs | ForEach-Object { ConvertTo-SpanTuiSccachePath $_ } | Sort-Object)
    $sameDirs = ($wanted -join "`n") -ceq ($actual -join "`n")
    $sameCache = (ConvertTo-SpanTuiSccachePath $server.Directory) -ceq (ConvertTo-SpanTuiSccachePath $config.Directory)
    if ($sameDirs -and $sameCache) { return }

    $self = (Get-SpanTuiCheckout -Root $Root).Root
    $busy = @($config.Checkouts | Where-Object { $_ -ine $self -and (Test-SpanTuiCheckoutBusy $_) })
    if ($busy.Count -gt 0) {
        Write-Host ("Compiler cache: not restarting sccache while a dev command runs in {0}; this build may miss objects of checkouts it does not know yet." -f ($busy -join ', ')) -ForegroundColor Yellow
        return
    }
    Write-Host ("Compiler cache: restarting sccache (port {0}) for {1} checkout(s)" -f $config.Port, $config.Checkouts.Count)
    & $sccache.Source --stop-server *> $null
    & $sccache.Source --start-server *> $null
    if ($LASTEXITCODE -ne 0) { Write-Host 'Compiler cache: sccache did not restart; compiling through it anyway.' -ForegroundColor Yellow }
}

# The configuration of the server SCCACHE_SERVER_PORT names (started when
# none listens): BaseDirs ($null when this sccache has none), Directory, Hits,
# Misses, Version. $null when sccache fails.
function Get-SpanTuiCompilerCacheServer([string]$Sccache) {
    $json = & $Sccache --show-stats --stats-format=json 2> $null
    if ($LASTEXITCODE -ne 0 -or -not $json) { return $null }
    $stats = ($json -join "`n") | ConvertFrom-Json
    $directory = if ("$($stats.cache_location)" -match '"(.+)"') { $Matches[1] -replace '\\\\', '\' } else { "$($stats.cache_location)" }
    $count = { param($counter) $total = 0; foreach ($p in $counter.counts.PSObject.Properties) { $total += [int]$p.Value }; $total }
    # Assigned, not taken from an if expression, which would turn [] into $null.
    $baseDirs = $null
    if ($stats.PSObject.Properties['basedirs']) { $baseDirs = @($stats.basedirs) }
    return [pscustomobject]@{
        BaseDirs = $baseDirs
        Directory = $directory
        Hits = & $count $stats.stats.cache_hits
        Misses = & $count $stats.stats.cache_misses
        Version = "$($stats.version)"
    }
}

# Configures the Ninja tree unless it is current. CMake cannot switch
# generators in place, so a tree another generator owns loses its cache first;
# objects are recompiled through sccache.
function Initialize-SpanTuiBuildTree {
    param(
        [Parameter(Mandatory = $true)][pscustomobject]$Toolchain,
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')][string]$BuildType = 'Debug',
        [string[]]$ExtraArgs = @(),
        [switch]$Force
    )
    # Always passed, so unsetting one reconfigures back to the default. The
    # compilers are pinned: CMake would take clang.exe from the LLVM folder
    # SPANTUI_LINKER=lld puts first on PATH.
    $debugInfo = if ([string]::IsNullOrWhiteSpace($env:SPANTUI_DEBUG_INFO)) { 'ON' } else { $env:SPANTUI_DEBUG_INFO }
    $arguments = @('-S', $SourceDir, '-B', $BuildDir, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_MAKE_PROGRAM=$($Toolchain.Ninja)", "-DCMAKE_C_COMPILER=$($Toolchain.CCompiler)",
        "-DCMAKE_CXX_COMPILER=$($Toolchain.Compiler)", "-DQt6_DIR=$($Toolchain.Qt6Dir)",
        "-DSPANTUI_LINKER=$env:SPANTUI_LINKER",
        "-DSPANTUI_DEBUG_INFO=$debugInfo") + $ExtraArgs
    $foreignQt = Test-SpanTuiCacheForeignQt -BuildDir $BuildDir -QtRoot $Toolchain.QtRoot
    if (-not $Force -and -not $foreignQt -and (Test-SpanTuiBuildTreeCurrent -BuildDir $BuildDir -Arguments $arguments)) {
        Write-Host "Using existing build directory: $BuildDir"
        return
    }
    if ((Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt')) -and -not (Test-SpanTuiNinjaCache $BuildDir)) {
        Write-Host "Build tree uses a non-Ninja generator; resetting CMake cache in $BuildDir" -ForegroundColor Yellow
        Remove-Item -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'), (Join-Path $BuildDir 'CMakeFiles') `
            -Recurse -Force -ErrorAction SilentlyContinue
    }
    elseif ($foreignQt) {
        Write-Host "Build tree was configured against another Qt; resetting CMake cache in $BuildDir" -ForegroundColor Yellow
        Remove-Item -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'), (Join-Path $BuildDir 'CMakeFiles') `
            -Recurse -Force -ErrorAction SilentlyContinue
    }
    elseif (Test-SpanTuiCacheForeignSource -BuildDir $BuildDir -SourceDir $SourceDir) {
        Write-Host "Build tree was configured for another source folder (moved checkout?); resetting CMake cache in $BuildDir" -ForegroundColor Yellow
        Remove-Item -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'), (Join-Path $BuildDir 'CMakeFiles') `
            -Recurse -Force -ErrorAction SilentlyContinue
    }
    Invoke-SpanTuiTimedStep "CMake configure ($BuildType)" {
        & $Toolchain.CMake @arguments
        if ($LASTEXITCODE -ne 0) { Fail "CMake configuration failed for $BuildDir." }
    }
    [System.IO.File]::WriteAllText((Join-Path $BuildDir '.spantui-configure'), ($arguments -join "`n"))
}

function Invoke-SpanTuiBuild {
    param(
        [Parameter(Mandatory = $true)][pscustomobject]$Toolchain,
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [string[]]$Target = @('SpanTui'),
        [int]$Jobs = 0
    )

    $targets = @($Target | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $args = @('--build', $BuildDir)
    if ($targets.Count -gt 0) {
        $args += @('--target') + $targets
    }
    if ($Jobs -gt 0) {
        $args += @('--parallel', "$Jobs")
    } else {
        $args += '--parallel'
    }

    & $Toolchain.CMake @args
    if ($LASTEXITCODE -ne 0) {
        $label = if ($targets.Count -eq 0) { 'default targets' } else { $targets -join ', ' }
        Fail "Build failed ($label)."
    }
}

# Run helpers and `dev diag` probes may only ever touch this workspace's
# SpanTui-test.exe / SpanTui-pre.exe. Matching is by full image path, so an
# installed SpanTui.exe or a foreign worktree is never selected.

function Get-SpanTuiTestProcesses {
    param([Parameter(Mandatory = $true)][string]$ExecutablePath)
    # Get-Process is much cheaper than Win32_Process CIM queries on every start.
    # Path can throw for elevated/protected processes — treat those as non-matches.
    $processName = [System.IO.Path]::GetFileNameWithoutExtension($ExecutablePath)
    @(Get-Process -Name $processName -ErrorAction SilentlyContinue | Where-Object {
        try {
            $_.Path -and ([string]::Compare($_.Path, $ExecutablePath, $true) -eq 0)
        } catch {
            $false
        }
    })
}

function Stop-SpanTuiTestInstances {
    param([Parameter(Mandatory = $true)][string]$ExecutablePath)
    $running = @(Get-SpanTuiTestProcesses -ExecutablePath $ExecutablePath)
    if ($running.Count -eq 0) { return }
    Write-Host ("Stopping {0} running test process(es) at {1}..." -f $running.Count, $ExecutablePath)
    foreach ($proc in $running) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    # Brief wait for the PE image unlock; poll instead of a fixed 400ms sleep.
    $deadline = [DateTime]::UtcNow.AddMilliseconds(800)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (@(Get-SpanTuiTestProcesses -ExecutablePath $ExecutablePath).Count -eq 0) { break }
        Start-Sleep -Milliseconds 50
    }
}

# Launches an image (shell-executed, so no pipes outlive a killed GUI child),
# waits up to $Seconds and stops it again. Alive = it was still running.
function Test-SpanTuiStaysAlive {
    param(
        [Parameter(Mandatory = $true)][string]$ExecutablePath,
        [string]$Arguments = '',
        [int]$Seconds = 5,
        [switch]$Hidden
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExecutablePath
    $psi.WorkingDirectory = Split-Path -Parent $ExecutablePath
    $psi.Arguments = $Arguments
    $psi.UseShellExecute = $true
    $psi.CreateNoWindow = [bool]$Hidden
    if ($Hidden) { $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden }
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc) { Fail "Failed to start $ExecutablePath" }
    $deadline = [DateTime]::UtcNow.AddSeconds([Math]::Max(1, $Seconds))
    while (-not $proc.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
    $alive = -not $proc.HasExited
    if ($alive) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        try { $proc.WaitForExit(3000) | Out-Null } catch { }
    }
    return [pscustomobject]@{
        Alive    = $alive
        Id       = $proc.Id
        ExitCode = $(if ($alive) { $null } else { $proc.ExitCode })
    }
}

function Format-SpanTuiAppArgs {
    param([string[]]$ArgsList)
    if (-not $ArgsList -or $ArgsList.Count -eq 0) { return '' }
    return ($ArgsList | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' '
}

# Last, so Fail and the other helpers exist when the file is malformed.
Import-SpanTuiDotEnv
