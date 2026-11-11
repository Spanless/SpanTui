# Development entry point behind dev.bat: maps `dev <command> [options]` onto
# the worker scripts beside this file and runs them in this console. Commands
# of one repository share its build tree, so a second invocation waits for
# the running one instead of racing it.
#
# Arguments are read from $args (no param block) so GNU-style options such as
# `-R` and `--` reach the parser below verbatim.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$usage = @'
Usage:
  dev build   [--config Debug|Release] [--all] [--jobs N] [--clean|--reconfigure]
  dev run     [--headless|--show-window] [--smoke] [-- app-args...]
  dev test    [-R regex]... [--label L] [--function name]... [--repeat N] [--build-only] [--list] [--coverage] [--jobs N] [--config C] [--reconfigure]
  dev package [--quick] [--clean] [--reconfigure] [--jobs N]
  dev release [--dry-run] [--jobs N]  (package master's tested head and publish GitHub release vX.Y.Z;
              --dry-run: report the gates, build and check the package, publish nothing)
  dev diag    [<probe> [args...]]  (no probe lists what is available)
  dev clean   (deletes .dev\: dev-run data, test TEMP/APPDATA, scratch)
  dev check   (lint scripts: docs, layout, QML style; no toolchain needed)
  dev ci      [--core [--base rev]] [--jobs N]  (what GitHub CI runs: check, build, full test suite;
              --core: core tests plus those the changes since rev touch, as pull requests into dev)
  dev worktree add <branch> [<base>] | list | remove <id|branch> | prune
              (parallel checkouts, each with its own build tree and .dev)
'@

$command = if ($args.Count -gt 0) { [string]$args[0] } else { 'help' }
$options = @($args | Select-Object -Skip 1 | ForEach-Object { [string]$_ })

# Each command lists its options: a flag maps to a worker switch, an option
# with a value to a worker parameter. `-R` repeats and its values are OR-ed
# into one CTest regex, so callers never need a literal `|` (cmd would read
# it as a pipe). `--function` also repeats and names QtTest functions to run.
$flags = @{
    build   = @{ '--all' = 'AllTargets'; '--clean' = 'Clean'; '--reconfigure' = 'Configure' }
    run     = @{ '--all' = 'AllTargets'; '--clean' = 'Clean'; '--reconfigure' = 'Configure'
                 '--headless' = 'NoWindow'; '--show-window' = 'ShowWindow'; '--smoke' = 'Smoke' }
    test    = @{ '--reconfigure' = 'Configure'; '--build-only' = 'BuildOnly'; '--list' = 'List'; '--coverage' = 'Coverage' }
    package = @{ '--quick' = 'PortableOnly'; '--clean' = 'Clean'; '--reconfigure' = 'ForceConfigure' }
    release = @{ '--dry-run' = 'DryRun' }
    clean   = @{}
    check   = @{}
    ci      = @{ '--core' = 'Core' }
}
$valued = @{
    build   = @{ '--config' = 'Configuration'; '--jobs' = 'Jobs' }
    run     = @{ '--config' = 'Configuration'; '--jobs' = 'Jobs' }
    test    = @{ '--config' = 'Configuration'; '--jobs' = 'Jobs'; '--repeat' = 'Repeat'; '--label' = 'Label' }
    package = @{ '--jobs' = 'Jobs' }
    release = @{ '--jobs' = 'Jobs' }
    clean   = @{}
    check   = @{}
    ci      = @{ '--jobs' = 'Jobs'; '--base' = 'Base' }
}
$workers = @{
    build   = 'start-worker.ps1'
    run     = 'start-worker.ps1'
    test    = 'test-worker.ps1'
    package = 'package-worker.ps1'
    release = 'release-worker.ps1'
    diag    = 'diag-worker.ps1'
    clean   = 'clean-worker.ps1'
    check   = 'ci-worker.ps1'
    ci      = 'ci-worker.ps1'
    worktree = 'worktree-worker.ps1'
}

if (-not $workers.ContainsKey($command)) {
    Write-Host $usage
    exit $(if ($command -in @('help', '--help', '-h', '/?')) { 0 } else { 2 })
}

$parameters = @{}
if ($command -eq 'diag') {
    # Probe arguments pass through verbatim; diag-worker owns their syntax.
    if ($options.Count -gt 0) { $parameters.Probe = $options[0] }
    if ($options.Count -gt 1) { $parameters.ProbeArgs = @($options | Select-Object -Skip 1) }
} elseif ($command -eq 'worktree') {
    if ($options.Count -gt 0) { $parameters.Action = $options[0] }
    if ($options.Count -gt 1) { $parameters.Arguments = @($options | Select-Object -Skip 1) }
} else {
    $regex = @()
    $functions = @()
    for ($i = 0; $i -lt $options.Count; ++$i) {
        $option = $options[$i]
        $hasValue = $i + 1 -lt $options.Count
        if ($flags[$command].ContainsKey($option)) {
            $parameters[$flags[$command][$option]] = $true
        } elseif ($valued[$command].ContainsKey($option) -and $hasValue) {
            $parameters[$valued[$command][$option]] = $options[++$i]
        } elseif ($command -eq 'test' -and $option -in @('-R', '--regex') -and $hasValue) {
            $regex += $options[++$i]
        } elseif ($command -eq 'test' -and $option -eq '--function' -and $hasValue) {
            $functions += $options[++$i]
        } elseif ($command -eq 'run' -and $option -eq '--') {
            $parameters.AppArgs = @($options | Select-Object -Skip ($i + 1))
            break
        } else {
            Write-Host "Unknown $command option: $option"
            Write-Host $usage
            exit 2
        }
    }
    if ($regex.Count -gt 0) { $parameters.Regex = $regex -join '|' }
    if ($functions.Count -gt 0) { $parameters.Function = $functions }
    if ($command -eq 'build') { $parameters.SkipLaunch = $true }
    if ($command -eq 'check') { $parameters.StaticOnly = $true }
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot '..\toolchain.ps1')
$mutex = New-SpanTuiDevMutex -Root $root
$owned = $false
$watch = [System.Diagnostics.Stopwatch]::StartNew()
$exitCode = 1
try {
    try {
        $owned = $mutex.WaitOne(0)
    } catch [System.Threading.AbandonedMutexException] {
        $owned = $true
    }
    if (-not $owned) {
        Write-Host '[dev] another dev command is running in this repository; waiting...'
        try {
            $owned = $mutex.WaitOne()
        } catch [System.Threading.AbandonedMutexException] {
            $owned = $true
        }
    }
    $global:LASTEXITCODE = 0
    & (Join-Path $PSScriptRoot $workers[$command]) @parameters
    $exitCode = $LASTEXITCODE
} catch {
    # Workers report through Fail (toolchain.ps1), which already printed it.
    Write-Host $_.Exception.Message
    $exitCode = 1
} finally {
    if ($owned) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
Write-Host ("[dev] {0} finished in {1:n1}s, exit {2}" -f $command, $watch.Elapsed.TotalSeconds, $exitCode)
exit $exitCode
