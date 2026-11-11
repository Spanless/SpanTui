# Shared helpers for `dev diag` probes — dot-source from a probe script:
#     . (Join-Path $PSScriptRoot 'common.ps1')
# Every probe is invoked by diag-worker.ps1 with the standard context
#   -Root <repo> -BuildDir <build tree> -AppPath <build tree>\SpanTui-test.exe -OutDir <dir>
# plus its own arguments, and reports a verdict by exit code (0 = OK, 1 = WARN).
# Process-boundary helpers (Get/Stop-SpanTuiTestProcesses, log paths,
# Format-SpanTuiAppArgs, Fail, Require-File) come from tools/toolchain.ps1.
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\..\toolchain.ps1')


function Start-SpanTuiTestApp {
    param(
        [Parameter(Mandatory = $true)][string]$AppPath,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [string[]]$AppArgs = @(),
        [hashtable]$Environment = @{}
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $AppPath
    $psi.WorkingDirectory = $WorkingDirectory
    # UseShellExecute=$false is required for EnvironmentVariables. The Qt window
    # still appears; CreateNoWindow only suppresses a console allocation.
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    foreach ($key in $Environment.Keys) {
        $psi.EnvironmentVariables[$key] = [string]$Environment[$key]
    }
    $argLine = Format-SpanTuiAppArgs $AppArgs
    if ($argLine) { $psi.Arguments = $argLine }
    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc) { Fail "Failed to start $AppPath" }
    return $proc
}

function Stop-SpanTuiTestApp {
    param($Process)
    if ($null -ne $Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        try { $Process.WaitForExit(3000) | Out-Null } catch { }
    }
}

function Copy-SpanTuiLaunchLog {
    param(
        [Parameter(Mandatory = $true)][string]$OutDir,
        [string]$Name = 'last-launch.log'
    )
    # Probes launch with the development data root (Get-SpanTuiDevDataArgs).
    $src = Join-Path (Get-SpanTuiDevPath -Name 'data') 'last-launch.log'
    if (-not (Test-Path -LiteralPath $src)) { return $null }
    $dst = Join-Path $OutDir $Name
    Copy-Item -LiteralPath $src -Destination $dst -Force
    return $dst
}

# GetThreadDescription gives stable names for Qt/C++ worker threads; the
# earliest-started thread is labelled MAIN (the GUI thread).

if (-not ('SpanTuiDiagThreadDesc' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class SpanTuiDiagThreadDesc {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    public static string Get(uint tid) {
        IntPtr h = OpenThread(0x0040, false, tid);
        if (h == IntPtr.Zero) return "";
        IntPtr p; string s = "";
        if (GetThreadDescription(h, out p) >= 0 && p != IntPtr.Zero) { s = Marshal.PtrToStringUni(p); LocalFree(p); }
        CloseHandle(h); return s;
    }
}
"@
}

function Get-SpanTuiThreadCpuSnapshot {
    param([Parameter(Mandatory = $true)]$Process)
    $map = @{}
    if (-not $Process.HasExited) {
        $Process.Refresh()
        foreach ($t in $Process.Threads) { $map[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds }
    }
    return $map
}

# Sleeps SliceSeconds between snapshots; returns Rows (per-thread CPU ms per
# slice, sorted by total desc) plus the real ElapsedSeconds actually sampled.
function Measure-SpanTuiThreadCpu {
    param(
        [Parameter(Mandatory = $true)]$Process,
        [Parameter(Mandatory = $true)][int]$Slices,
        [Parameter(Mandatory = $true)][int]$SliceSeconds
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $snaps = @()
    $snaps += ,(Get-SpanTuiThreadCpuSnapshot $Process)
    for ($i = 0; $i -lt $Slices -and -not $Process.HasExited; $i++) {
        Start-Sleep -Seconds $SliceSeconds
        $snaps += ,(Get-SpanTuiThreadCpuSnapshot $Process)
    }
    $sw.Stop()

    $names = @{}
    if (-not $Process.HasExited) {
        $mainTid = ($Process.Threads | Sort-Object StartTime | Select-Object -First 1).Id
        foreach ($t in $Process.Threads) {
            $n = [SpanTuiDiagThreadDesc]::Get([uint32]$t.Id)
            if ($t.Id -eq $mainTid) { $n = "MAIN $n" }
            $names[$t.Id] = @{ Name = $n.Trim(); Start = $t.StartTime.ToString('HH:mm:ss.f') }
        }
    }

    $last = $snaps[$snaps.Count - 1]
    $rows = foreach ($tid in $last.Keys) {
        $cells = @()
        for ($i = 1; $i -lt $snaps.Count; $i++) {
            $a = if ($snaps[$i - 1].ContainsKey($tid)) { $snaps[$i - 1][$tid] } else { 0 }
            $b = if ($snaps[$i].ContainsKey($tid)) { $snaps[$i][$tid] } else { $a }
            $cells += [math]::Round($b - $a)
        }
        $total = ($cells | Measure-Object -Sum).Sum
        if ($total -gt 0) {
            $meta = if ($names.ContainsKey($tid)) { $names[$tid] } else { @{ Name = ''; Start = '' } }
            [pscustomobject]@{
                Tid     = $tid
                TotalMs = $total
                Slices  = ($cells -join ' ')
                Start   = $meta.Start
                Name    = $meta.Name
            }
        }
    }
    return [pscustomobject]@{
        Rows           = @($rows | Sort-Object TotalMs -Descending)
        ElapsedSeconds = $sw.Elapsed.TotalSeconds
    }
}


if (-not ('SpanTuiDiagGuiResources' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class SpanTuiDiagGuiResources {
    [DllImport("user32.dll")] public static extern int GetGuiResources(IntPtr hProcess, int uiFlags);
}
"@
}

function Get-SpanTuiGuiObjectCounts {
    param([Parameter(Mandatory = $true)]$Process)
    try {
        return [pscustomobject]@{
            Gdi  = [SpanTuiDiagGuiResources]::GetGuiResources($Process.Handle, 0)
            User = [SpanTuiDiagGuiResources]::GetGuiResources($Process.Handle, 1)
        }
    } catch {
        return [pscustomobject]@{ Gdi = -1; User = -1 }
    }
}

# Writes report text to <OutDir>\<Name> AND echoes it, so `dev diag` output and
# the persisted artifact stay identical.

function Write-SpanTuiDiagReport {
    param(
        [Parameter(Mandatory = $true)][string]$OutDir,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines,
        [string]$Name = 'report.txt'
    )
    $path = Join-Path $OutDir $Name
    $Lines | Set-Content -LiteralPath $path -Encoding utf8
    foreach ($line in $Lines) { Write-Host $line }
    return $path
}
