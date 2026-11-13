# `dev worktree`: parallel checkouts of this repository, one per task. Each
# worktree has its own branch, .dev\ and build tree (<SPANTUI_BUILD_ROOT>\wt-<id>,
# or its own target\build) and reads machine settings from the main
# checkout's .env, so a second task never needs a stash or a branch switch.
# add, remove and prune also tell the shared sccache server the new set of
# checkouts (Use-SpanTuiCompilerCache in toolchain.ps1).
#
#   add <branch> [<base>]   check out <branch> in a new worktree; a branch that
#                           does not exist yet is created from <base> (default dev)
#   list                    checkouts, branches and build trees
#   remove <id|branch>      remove a clean worktree and its build tree (the branch stays)
#   prune                   forget deleted worktrees and remove build trees no
#                           worktree owns any more (e.g. after deleting a folder)
[CmdletBinding()]
param(
    [string]$Action = 'list',
    [string[]]$Arguments = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\toolchain.ps1')

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$main = (Get-SpanTuiCheckout -Root $root).MainRoot

function Invoke-Git([string[]]$GitArgs) {
    $output = & git -C $main @GitArgs
    if ($LASTEXITCODE -ne 0) { Fail "git $($GitArgs -join ' ') failed." }
    return @($output)
}

# Linked worktrees as git records them: id (admin folder), path and branch.
function Get-LinkedWorktrees {
    $byPath = @{}
    $current = $null
    foreach ($line in @(Invoke-Git @('worktree', 'list', '--porcelain')) + '') {
        if ($line -like 'worktree *') {
            $current = [ordered]@{ Path = [System.IO.Path]::GetFullPath($line.Substring(9)).TrimEnd('\'); Branch = '(detached)' }
        } elseif ($line -like 'branch *' -and $current) {
            $current.Branch = $line.Substring(7) -replace '^refs/heads/', ''
        } elseif ($line -eq '' -and $current) {
            $byPath[$current.Path.ToLowerInvariant()] = $current
            $current = $null
        }
    }
    $worktrees = @(Get-SpanTuiLinkedCheckouts -MainRoot $main)
    foreach ($worktree in $worktrees) {
        $known = $byPath[$worktree.Path.ToLowerInvariant()]
        $worktree | Add-Member -NotePropertyName Branch -NotePropertyValue $(if ($known) { $known.Branch } else { '(unknown)' })
    }
    return $worktrees
}

function Get-BuildRoot {
    if ([string]::IsNullOrWhiteSpace($env:SPANTUI_BUILD_ROOT)) { return $null }
    return Resolve-SpanTuiRepoPath $main $env:SPANTUI_BUILD_ROOT
}

# A build tree the tooling configured: never delete anything else.
function Test-SpanTuiBuildTree([string]$Path) {
    return (Test-Path -LiteralPath (Join-Path $Path '.spantui-configure') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $Path 'CMakeCache.txt') -PathType Leaf)
}

# wt-* build trees under the build root whose worktree is gone. Folders the
# tooling did not configure are never touched.
function Get-OrphanBuildTrees([string]$BuildRoot, [string[]]$Owned) {
    return @(Get-ChildItem -LiteralPath $BuildRoot -Directory -Filter 'wt-*' |
        Where-Object { $_.Name -notin $Owned -and (Test-SpanTuiBuildTree $_.FullName) })
}

function Remove-BuildTree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return }
    if (-not (Test-SpanTuiBuildTree $Path)) {
        Write-Host "Kept $Path`: it does not look like a SpanTui build tree." -ForegroundColor Yellow
        return
    }
    Write-Host "Removing build tree $Path"
    Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $Path) { Fail "Could not remove all of $Path; close programs using it and run dev worktree prune." }
}

function Find-Worktree([string]$Key) {
    $match = @(Get-LinkedWorktrees | Where-Object {
        $_.Id -ieq $Key -or $_.Name -ieq $Key -or $_.Branch -ceq $Key -or
        $_.Path -ieq (Resolve-SpanTuiRepoPath (Get-Location).Path $Key)
    })
    if ($match.Count -eq 0) { Fail "No linked worktree is named $Key; see dev worktree list." }
    if ($match.Count -gt 1) { Fail "$Key matches several worktrees; use the id from dev worktree list." }
    return $match[0]
}

switch ($Action) {
    'add' {
        if ($Arguments.Count -lt 1 -or $Arguments.Count -gt 2) { Fail 'Usage: dev worktree add <branch> [<base>]' }
        $branch = $Arguments[0]
        $base = if ($Arguments.Count -gt 1) { $Arguments[1] } else { 'dev' }
        & git -C $main check-ref-format --branch $branch *> $null
        if ($LASTEXITCODE -ne 0) { Fail "$branch is not a valid branch name." }
        # Settle the output variables before anything is created.
        [void](Resolve-SpanTuiBuildDirectory -Root $main)
        $path = Join-Path (Resolve-SpanTuiWorktreeRoot -Root $main) ($branch -replace '[\\/]', '-')
        if (Test-Path -LiteralPath $path) { Fail "$path already exists." }
        & git -C $main show-ref --verify --quiet "refs/heads/$branch"
        if ($LASTEXITCODE -eq 0) {
            [void](Invoke-Git @('worktree', 'add', $path, $branch))
        } else {
            [void](Invoke-Git @('worktree', 'add', '-b', $branch, $path, $base))
        }
        $checkout = Get-SpanTuiCheckout -Root $path
        # Now, while the other checkouts are most likely idle.
        Use-SpanTuiCompilerCache
        Write-Host "Worktree $($checkout.Name) on $branch" -ForegroundColor Green
        Write-Host "  checkout: $path"
        Write-Host "  build:    $(Resolve-SpanTuiBuildDirectory -Root $path)"
        Write-Host "  data:     $(Join-Path $path '.dev\data')"
        Write-Host '  Run dev.bat there; its first build configures a new tree (with sccache, it reuses what the other checkouts compiled).'
    }
    'list' {
        $buildRoot = Get-BuildRoot
        $mainBranch = & git -C $main branch --show-current
        if (-not $mainBranch) { $mainBranch = '(detached)' }
        $rows = @([pscustomobject]@{ Name = 'main'; Branch = $mainBranch; Path = $main; Exists = $true })
        $rows += @(Get-LinkedWorktrees | Sort-Object Name)
        foreach ($row in $rows) {
            $state = if ($row.Exists) { '' } else { '  (folder missing: dev worktree prune)' }
            Write-Host ("{0,-28} {1}{2}" -f $row.Name, $row.Branch, $state)
            Write-Host ("    checkout: {0}" -f $row.Path)
            if ($row.Exists) {
                $build = Resolve-SpanTuiBuildDirectory -Root $row.Path
                $built = if (Test-Path -LiteralPath (Join-Path $build 'build.ninja')) { '' } else { '  (not built yet)' }
                Write-Host ("    build:    {0}{1}" -f $build, $built)
            }
        }
        if ($buildRoot -and (Test-Path -LiteralPath $buildRoot -PathType Container)) {
            $owned = @($rows | ForEach-Object { $_.Name })
            foreach ($orphan in @(Get-OrphanBuildTrees $buildRoot $owned)) {
                Write-Host ("Orphaned build tree {0} (dev worktree prune removes it)" -f $orphan.FullName) -ForegroundColor Yellow
            }
        }
    }
    'remove' {
        if ($Arguments.Count -ne 1) { Fail 'Usage: dev worktree remove <id|branch>' }
        $worktree = Find-Worktree $Arguments[0]
        if ([string]::Compare($worktree.Path, $root, $true) -eq 0) { Fail 'Run dev worktree remove from another checkout than the one being removed.' }
        $build = if ($worktree.Exists) { Resolve-SpanTuiBuildDirectory -Root $worktree.Path } else { $null }
        $mutex = New-SpanTuiDevMutex -Root $worktree.Path
        $owned = $false
        try {
            try { $owned = $mutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $owned = $true }
            if (-not $owned) { Fail "A dev command is running in $($worktree.Path); wait for it to finish." }
            if ($build) {
                Stop-SpanTuiTestInstances -ExecutablePath (Join-Path $build 'SpanTui-test.exe')
                Stop-SpanTuiTestInstances -ExecutablePath (Join-Path $build 'release\SpanTui-pre.exe')
            }
            # No --force: git refuses modified or untracked files, which are
            # someone's work. Ignored output (.dev\, target\) goes with it.
            & git -C $main worktree remove $worktree.Path
            if ($LASTEXITCODE -ne 0) { Fail "git kept $($worktree.Path): commit or discard its changes first." }
        } finally {
            if ($owned) { $mutex.ReleaseMutex() }
            $mutex.Dispose()
        }
        $buildRoot = Get-BuildRoot
        if ($buildRoot) { Remove-BuildTree (Join-Path $buildRoot $worktree.Name) }
        Use-SpanTuiCompilerCache
        Write-Host "Removed worktree $($worktree.Name). Branch $($worktree.Branch) is kept; delete it with git branch -d $($worktree.Branch) once merged." -ForegroundColor Green
    }
    'prune' {
        [void](Invoke-Git @('worktree', 'prune'))
        Use-SpanTuiCompilerCache
        $buildRoot = Get-BuildRoot
        if (-not $buildRoot -or -not (Test-Path -LiteralPath $buildRoot -PathType Container)) {
            Write-Host 'Worktree records pruned; no SPANTUI_BUILD_ROOT build trees to check.'
            return
        }
        $owned = @(Get-LinkedWorktrees | ForEach-Object { $_.Name })
        $orphans = @(Get-OrphanBuildTrees $buildRoot $owned)
        foreach ($orphan in $orphans) { Remove-BuildTree $orphan.FullName }
        Write-Host ("Pruned: {0} orphaned build tree(s) under {1}." -f $orphans.Count, $buildRoot)
    }
    default {
        Fail "Unknown worktree action: $Action (add, list, remove, prune)."
    }
}
