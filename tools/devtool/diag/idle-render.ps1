# Idle render health: scene-graph frames / dirty nodes / per-thread CPU while the app sits untouched.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$AppPath,
    [Parameter(Mandatory = $true)][string]$OutDir,
    # Warmup must outlast the 30 s IdleGovernor input timeout — otherwise the
    # "steady" window still contains active-phase sampling and the verdict
    # thresholds misread it as idle.
    [int]$WarmupSeconds = 35,
    [int]$IdleSeconds = 20,
    [int]$SliceSeconds = 5,
    [double]$MaxIdleFps = 5,
    [double]$MaxIdleCpuPercent = 3
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

$preIdle = $WarmupSeconds -lt 32
if ($preIdle) {
    Write-Host "warning: warmup ${WarmupSeconds}s < 32s; the steady window includes pre-idle (active-phase) samples." -ForegroundColor Yellow
}
Write-Host "idle-render: launching $AppPath with scene-graph logging."
Write-Host "Do NOT type or move the mouse over the window — input would produce legitimate frames." -ForegroundColor Yellow

Stop-SpanTuiTestInstances -ExecutablePath $AppPath
$proc = Start-SpanTuiTestApp -AppPath $AppPath -WorkingDirectory $Root `
    -AppArgs @(Get-SpanTuiDevDataArgs -Root $Root) `
    -Environment @{ QT_LOGGING_RULES = 'qt.scenegraph.general=true;qt.scenegraph.time.renderloop=true;qt.quick.dirty=true' }
Write-Host "pid $($proc.Id): warmup ${WarmupSeconds}s, then ~${IdleSeconds}s sampling in ${SliceSeconds}s slices..."

$exitCode = 0
$sampledSeconds = 0.0
$threadRows = @()
$cpuDeltaMs = 0.0
try {
    Start-Sleep -Seconds $WarmupSeconds
    if ($proc.HasExited) { Fail "SpanTui-test exited during warmup (code $($proc.ExitCode))" }
    $proc.Refresh()
    $cpuStartMs = $proc.TotalProcessorTime.TotalMilliseconds
    $slices = [Math]::Max(1, [int][Math]::Ceiling($IdleSeconds / $SliceSeconds))
    $measure = Measure-SpanTuiThreadCpu -Process $proc -Slices $slices -SliceSeconds $SliceSeconds
    $threadRows = $measure.Rows
    $sampledSeconds = $measure.ElapsedSeconds
    $proc.Refresh()
    $cpuDeltaMs = $proc.TotalProcessorTime.TotalMilliseconds - $cpuStartMs
    if ($proc.HasExited) { Fail "SpanTui-test exited during sampling (code $($proc.ExitCode))" }
} finally {
    Stop-SpanTuiTestApp -Process $proc
    Start-Sleep -Seconds 2   # let the message handler flush last-launch.log
}

$logPath = Copy-SpanTuiLaunchLog -OutDir $OutDir -Name 'scenegraph.log'
if (-not $logPath) { Fail '.dev\data\last-launch.log not found after the run' }

# Cumulative frame intervals approximate wall time; a frame belongs to the
# steady window once the running total passes the warmup. The log is
# chronological, so sync/dirty lines after the first steady frame's position
# are steady-state too.
$frameRe = [regex]'syncAndRender: start, elapsed since last call: (\d+) ms'
$steadyAfterMs = [int64]$WarmupSeconds * 1000
$cumMs = [int64]0
$steadyIndex = -1
$totalFrames = 0; $steadyFrames = 0; $zeroGap = 0
$totalSyncs = 0; $steadySyncs = 0
$intervals = @{}
$dirtyCounts = @{}
$general = [System.Collections.Generic.List[string]]::new()
$lineIndex = 0
foreach ($line in (Get-Content -LiteralPath $logPath)) {
    if ($line.IndexOf('qt.scenegraph.general', [StringComparison]::Ordinal) -ge 0) {
        $general.Add($line.Trim())
    }
    $m = $frameRe.Match($line)
    if ($m.Success) {
        $elapsed = [int]$m.Groups[1].Value
        $cumMs += $elapsed
        $totalFrames++
        if ($cumMs -ge $steadyAfterMs) {
            if ($steadyIndex -lt 0) { $steadyIndex = $lineIndex }
            $steadyFrames++
            if ($elapsed -eq 0) { $zeroGap++ }
            $bucket = if ($elapsed -eq 0) { '0ms' } elseif ($elapsed -le 8) { '1-8ms' } elseif ($elapsed -le 20) { '9-20ms' } else { '>20ms' }
            $intervals[$bucket] = 1 + [int]$intervals[$bucket]
        }
    } elseif ($line.IndexOf('updateDirtyNodes', [StringComparison]::Ordinal) -ge 0) {
        $totalSyncs++
        if ($steadyIndex -ge 0) { $steadySyncs++ }
    } elseif ($steadyIndex -ge 0 -and $line -match 'qt\.quick\.dirty:\s+QSGNode:\s*(.+)$') {
        $key = ($Matches[1] -replace '0x[0-9a-fA-F]+', '0x…').Trim()
        $dirtyCounts[$key] = 1 + [int]$dirtyCounts[$key]
    }
    $lineIndex++
}

$steadyFps = if ($sampledSeconds -gt 0) { [math]::Round($steadyFrames / $sampledSeconds, 2) } else { 0 }
$cpuPct = if ($sampledSeconds -gt 0) { [math]::Round(100.0 * $cpuDeltaMs / ($sampledSeconds * 1000), 2) } else { 0 }

$report = [System.Collections.Generic.List[string]]::new()
$report.Add("idle-render report  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$report.Add("app:     $AppPath")
$report.Add("sampled: warmup ${WarmupSeconds}s + $([math]::Round($sampledSeconds, 1))s steady (slices of ${SliceSeconds}s)")
$report.Add('')
$report.Add('== scene graph ==')
foreach ($g in ($general | Select-Object -First 20)) { $report.Add("  $g") }
$report.Add('')
$report.Add("frames total: $totalFrames   steady-state: $steadyFrames  (${steadyFps} fps)")
$report.Add("zero-interval frames in steady state: $zeroGap")
$report.Add("steady interval histogram: " + (@('0ms', '1-8ms', '9-20ms', '>20ms') | ForEach-Object {
    "$_=$([int]$intervals[$_])"
}) -join '  ')
$report.Add("GUI syncs total: $totalSyncs   steady: $steadySyncs")
if ($steadyFrames -gt 20 -and $steadySyncs -eq 0) {
    $report.Add('hint: frames >> syncs with no dirty items => a render-thread Animator is driving the loop')
}
$report.Add('')
$report.Add('== steady-state dirty nodes (top 10) ==')
$top = @($dirtyCounts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 10)
if ($top.Count -gt 0) {
    foreach ($d in $top) { $report.Add(("{0,6}  {1}" -f $d.Value, $d.Key)) }
} else {
    $report.Add('  (none)')
}
$report.Add('')
$report.Add("== per-thread CPU (ms per ${SliceSeconds}s slice, steady window) ==")
if ($threadRows.Count -gt 0) {
    $table = ($threadRows | Format-Table Tid, TotalMs, Slices, Start, Name -AutoSize | Out-String -Width 200).TrimEnd()
    foreach ($l in ($table -split "`r?`n")) { $report.Add("  $l") }
} else {
    $report.Add('  (no thread consumed CPU)')
}
$report.Add('')
$report.Add(("process CPU in steady window: {0:n0} ms over {1:n1}s = {2}% of one core" -f $cpuDeltaMs, $sampledSeconds, $cpuPct))

$reasons = @()
if ($preIdle) {
    $report.Add('note: warmup below the 30 s idle threshold — steady-window numbers reflect partly active-phase sampling')
    $report.Add('')
}
if ($steadyFps -gt $MaxIdleFps) { $reasons += "steady fps $steadyFps exceeds $MaxIdleFps" }
if ($steadyFrames -gt 20 -and ($zeroGap / $steadyFrames) -gt 0.05) {
    $reasons += "$zeroGap/$steadyFrames zero-interval frames (render busy-loop)"
}
if ($cpuPct -gt $MaxIdleCpuPercent) { $reasons += "CPU $cpuPct% of one core exceeds $MaxIdleCpuPercent%" }
$report.Add('')
if ($reasons.Count -eq 0) {
    $report.Add('PERF_IDLE_OK')
} else {
    $report.Add('PERF_IDLE_WARN: ' + ($reasons -join '; '))
    $exitCode = 1
}

$reportPath = Write-SpanTuiDiagReport -OutDir $OutDir -Lines $report
Write-Host ''
Write-Host "report: $reportPath"
Write-Host "log:    $logPath"
exit $exitCode
