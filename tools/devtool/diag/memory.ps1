# Memory timeline: WorkingSet / Private / Paged / handles / GDI+USER objects sampled per slice.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$AppPath,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$WarmupSeconds = 5,
    [int]$Seconds = 60,
    [int]$SliceSeconds = 10,
    # WARN when private bytes grow by more than this across the sampled window.
    [double]$MaxPrivateGrowthMB = 128
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

Write-Host "memory: launching $AppPath (headless)"
Stop-SpanTuiTestInstances -ExecutablePath $AppPath
# Headless: process metrics do not need a window, so no GUI pops up.
$proc = Start-SpanTuiTestApp -AppPath $AppPath -WorkingDirectory $Root -AppArgs (@('--headless') + (Get-SpanTuiDevDataArgs -Root $Root))
Write-Host "pid $($proc.Id): warmup ${WarmupSeconds}s, then ~${Seconds}s sampling in ${SliceSeconds}s slices..."

$exitCode = 0
$slices = [Math]::Max(1, [int][Math]::Ceiling($Seconds / $SliceSeconds))
$rows = [System.Collections.Generic.List[object]]::new()
try {
    Start-Sleep -Seconds $WarmupSeconds
    if ($proc.HasExited) { Fail "SpanTui-test exited during warmup (code $($proc.ExitCode))" }
    for ($i = 0; $i -lt $slices -and -not $proc.HasExited; $i++) {
        Start-Sleep -Seconds $SliceSeconds
        $proc.Refresh()
        $gui = Get-SpanTuiGuiObjectCounts -Process $proc
        $rows.Add([pscustomobject]@{
            Slice      = $i + 1
            WorkSetMB  = [math]::Round($proc.WorkingSet64 / 1MB, 1)
            PrivateMB  = [math]::Round($proc.PrivateMemorySize64 / 1MB, 1)
            PagedMB    = [math]::Round($proc.PagedMemorySize64 / 1MB, 1)
            VirtualMB  = [math]::Round($proc.VirtualMemorySize64 / 1MB, 1)
            Handles    = $proc.HandleCount
            Threads    = $proc.Threads.Count
            GdiObjects = $gui.Gdi
            UserObjects = $gui.User
        })
    }
    if ($proc.HasExited) { Fail "SpanTui-test exited during sampling (code $($proc.ExitCode))" }
} finally {
    Stop-SpanTuiTestApp -Process $proc
}

$report = [System.Collections.Generic.List[string]]::new()
$report.Add("memory report  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$report.Add("app:     $AppPath")
$report.Add("sampled: warmup ${WarmupSeconds}s + $($rows.Count) slice(s) of ${SliceSeconds}s")
$report.Add('')
$table = ($rows | Format-Table -AutoSize | Out-String -Width 200).TrimEnd()
foreach ($l in ($table -split "`r?`n")) { $report.Add("  $l") }
$report.Add('')

$reasons = @()
if ($rows.Count -ge 2) {
    $first = $rows[0]; $last = $rows[$rows.Count - 1]
    $report.Add('deltas over sampled window:')
    foreach ($p in 'WorkSetMB', 'PrivateMB', 'PagedMB', 'VirtualMB', 'Handles', 'Threads', 'GdiObjects', 'UserObjects') {
        $report.Add(("  {0,-12} {1} -> {2}  ({3:+#.#;-#.#;0})" -f $p, $first.$p, $last.$p, ($last.$p - $first.$p)))
    }
    $report.Add('')
    $privateGrowth = $last.PrivateMB - $first.PrivateMB
    if ($privateGrowth -gt $MaxPrivateGrowthMB) {
        $reasons += "private bytes grew $([math]::Round($privateGrowth, 1)) MB > $MaxPrivateGrowthMB MB"
    }
}
if ($reasons.Count -eq 0) {
    $report.Add('MEMORY_OK')
} else {
    $report.Add('MEMORY_WARN: ' + ($reasons -join '; '))
    $exitCode = 1
}

$reportPath = Write-SpanTuiDiagReport -OutDir $OutDir -Lines $report
Write-Host ''
Write-Host "report: $reportPath"
exit $exitCode
