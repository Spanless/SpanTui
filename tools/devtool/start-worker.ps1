[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Configure,
    [switch]$SkipLaunch,
    # Build, launch, wait, assert still alive (or clean exit). Then kill the smoke process.
    # Use this in automation so packaging never ships a binary that fails to start.
    [switch]$Smoke,
    [int]$SmokeSeconds = 5,
    # Do not show the Qt main window (passes --headless). Default for -Smoke.
    [switch]$NoWindow,
    # Force a visible window even under -Smoke.
    [switch]$ShowWindow,
    [switch]$AllTargets,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Debug',
    [int]$Jobs = 0,
    [string[]]$AppArgs = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\toolchain.ps1')

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Resolve-SpanTuiBuildDirectory -Root $root
# Local start image is SpanTui-test; package image is SpanTui-pre.
# Never overwrite the user's installed SpanTui.exe or a running interactive session.
$appName = 'SpanTui-test'
$app = Join-Path $build "$appName.exe"

$startSw = [System.Diagnostics.Stopwatch]::StartNew()

$toolchain = Resolve-SpanTuiToolchain
Use-SpanTuiToolchainPath $toolchain

function Show-LaunchDiagnostics {
    $data = Get-SpanTuiDevPath -Name 'data' -Root $root
    foreach ($log in @(@{ Path = (Join-Path $data 'last-launch.log'); Lines = 80 },
                       @{ Path = (Join-Path $data 'crash-latest.log'); Lines = 120 })) {
        if (Test-Path -LiteralPath $log.Path) {
            Write-Host "---- $($log.Path) ----" -ForegroundColor Yellow
            Get-Content -LiteralPath $log.Path -ErrorAction SilentlyContinue |
                Select-Object -Last $log.Lines | ForEach-Object { Write-Host $_ }
        }
    }
}

# Redirected starts carry the variables a GUI-subsystem exe needs to log to
# stderr at all.
function Start-RedirectedApp([switch]$Hidden) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $app
    $psi.WorkingDirectory = $build
    $psi.Arguments = Format-SpanTuiAppArgs $effectiveAppArgs
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = [bool]$Hidden
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables['QT_ASSUME_STDERR_HAS_CONSOLE'] = '1'
    $psi.EnvironmentVariables['QT_FORCE_STDERR_LOGGING'] = '1'
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc) {
        Fail "Failed to start $app"
    }
    return $proc
}

# Smoke defaults to headless so automation does not flash a GUI. -ShowWindow opts out.
$useHeadless = $NoWindow -or ($Smoke -and -not $ShowWindow)
$effectiveAppArgs = @($AppArgs)
if ($useHeadless -and -not ($effectiveAppArgs -contains '--headless' -or $effectiveAppArgs -contains '--no-window')) {
    $effectiveAppArgs = @('--headless') + $effectiveAppArgs
}
# `dev run` / smoke never touch the live product root: their data root is
# the checkout's .dev\data unless the caller names another --data-dir.
if ($effectiveAppArgs -notcontains '--data-dir') {
    $effectiveAppArgs += Get-SpanTuiDevDataArgs -Root $root
}
# A workspace smoke must not depend on, migrate, or lock the user's live
# workspace data. Isolation is independent from the screenshot harness: a
# visible smoke must remain alive for the normal lifetime check and must not
# inherit the screenshot timer's auto-exit behavior.
if ($Smoke -and $effectiveAppArgs -notcontains '--isolated-workspace' -and
    -not @($effectiveAppArgs | Where-Object { $_.StartsWith('--workspace-shot=') }).Count) {
    $effectiveAppArgs += '--isolated-workspace'
}

if ($Clean -and (Test-Path -LiteralPath $build)) {
    Write-Host "Removing build directory: $build"
    Remove-Item -LiteralPath $build -Recurse -Force
}

Initialize-SpanTuiBuildTree -Toolchain $toolchain -SourceDir $root -BuildDir $build `
    -BuildType $Configuration -Force:$Configure

# Windows cannot overwrite a running .exe; stop instances of THIS build output only.
Stop-SpanTuiTestInstances -ExecutablePath $app

Invoke-SpanTuiTimedStep 'Refresh product icon from SVG' {
    Invoke-SpanTuiBuild -Toolchain $toolchain -BuildDir $build `
        -Target 'spantui_brand_icon_refresh' -Jobs $Jobs
}
$target = if ($AllTargets) { '' } else { 'SpanTui' }
$targetLabel = if ($AllTargets) { 'all targets' } else { "SpanTui ($appName)" }
Invoke-SpanTuiTimedStep "Build $targetLabel" {
    Invoke-SpanTuiBuild -Toolchain $toolchain -BuildDir $build -Target $target -Jobs $Jobs
}
Require-File $app 'SpanTui-test executable'

if ($SkipLaunch -and -not $Smoke) {
    $startSw.Stop()
    Write-Host ("Build complete in {0:n1}s: {1}" -f $startSw.Elapsed.TotalSeconds, $app) -ForegroundColor Green
    exit 0
}

if ($Smoke) {
    $modeNote = if ($useHeadless) { 'headless' } else { 'with window' }
    Write-Host "Smoke-launching $appName for $SmokeSeconds s ($modeNote)..."
    # Diagnostics come from last-launch.log / crash-latest.log, not pipes.
    $result = Test-SpanTuiStaysAlive -ExecutablePath $app -Arguments (Format-SpanTuiAppArgs $effectiveAppArgs) `
        -Seconds $SmokeSeconds -Hidden:$useHeadless
    if (-not $result.Alive) {
        Write-Host "SMOKE_FAIL: $appName exited early with code $($result.ExitCode)" -ForegroundColor Red
        Show-LaunchDiagnostics
        exit 1
    }
    $startSw.Stop()
    Write-Host ("SMOKE_OK in {0:n1}s: {1} stayed alive for {2}s (pid was {3}, {4})" -f `
        $startSw.Elapsed.TotalSeconds, $appName, $SmokeSeconds, $result.Id, $modeNote) -ForegroundColor Green
    exit 0
}

if ($useHeadless) {
    Write-Host "Starting $appName (headless / no window)..."
    $proc = Start-RedirectedApp -Hidden
    # Detach readers so the process is not killed when this script exits; logs stay in last-launch.log.
    $null = $proc.StandardOutput.ReadToEndAsync()
    $null = $proc.StandardError.ReadToEndAsync()
    Write-Host ("Launched headless pid {0} in {1:n1}s (kill via Task Manager or Stop-Process)" -f `
        $proc.Id, $startSw.Elapsed.TotalSeconds) -ForegroundColor DarkGray
    exit 0
}

# Attached launch: stream the child's stdout/stderr into this console and wait
# for exit, so a failed launch surfaces here instead of vanishing silently.
Write-Host "Starting $appName (attached — output streams here; Ctrl+C detaches)..."
$proc = Start-RedirectedApp
# Event scriptblocks run on threadpool threads where Windows PowerShell has no
# runspace (crashes the worker); CopyToAsync pumps the pipes in pure .NET.
$null = $proc.StandardOutput.BaseStream.CopyToAsync([Console]::OpenStandardOutput())
$null = $proc.StandardError.BaseStream.CopyToAsync([Console]::OpenStandardError())
$proc.WaitForExit()
$code = $proc.ExitCode
$startSw.Stop()
if ($code -eq 0) {
    Write-Host ("$appName exited cleanly after {0:n1}s" -f $startSw.Elapsed.TotalSeconds) -ForegroundColor DarkGray
    exit 0
}
Write-Host "$appName exited with code $code" -ForegroundColor Red
Show-LaunchDiagnostics
exit $code
