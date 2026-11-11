# `dev diag <probe> [args...]` runner. Probes are scripts under
# tools/devtool/diag/<probe>.ps1; each declares the standard context params
# (-Root -BuildDir -AppPath -OutDir) plus its own options. Artifacts land in
# <build>\diag\<probe>-<timestamp>\ and the probe's exit code is propagated.
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Probe,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ProbeArgs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\toolchain.ps1')

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$probeDir = Join-Path $PSScriptRoot 'diag'

function Get-DiagProbes {
    @(Get-ChildItem -LiteralPath $probeDir -Filter '*.ps1' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne 'common.ps1' } |
        Sort-Object Name)
}

function Show-DiagUsage {
    Write-Host 'Usage: dev diag <probe> [probe-args...]'
    Write-Host ''
    Write-Host 'Probes (tools/devtool/diag/*.ps1 — the file name is the probe name):'
    foreach ($p in Get-DiagProbes) {
        $synopsis = ''
        foreach ($line in (Get-Content -LiteralPath $p.FullName -TotalCount 5)) {
            if ($line -match '^\s*#\s*(.+)$') { $synopsis = $Matches[1]; break }
        }
        Write-Host ("  {0,-14} {1}" -f $p.BaseName, $synopsis)
    }
    Write-Host ''
    Write-Host 'Artifacts: <build>\diag\<probe>-<timestamp>\  (<build> = target\build or <SPANTUI_BUILD_ROOT>\main|wt-<id>)'
    Write-Host 'Boundary:  only this workspace''s <build>\SpanTui-test.exe is ever started or stopped.'
}

if ([string]::IsNullOrWhiteSpace($Probe) -or $Probe -in @('list', '--list', '-list', '--help', '-h', '-?', '/?')) {
    Show-DiagUsage
    exit 0
}
if ($Probe -notmatch '^[a-zA-Z0-9][a-zA-Z0-9_-]*$') {
    Write-SpanError "Invalid probe name: $Probe"
    exit 2
}
$probeScript = Join-Path $probeDir "$Probe.ps1"
if (-not (Test-Path -LiteralPath $probeScript -PathType Leaf)) {
    Write-SpanError "Unknown probe: $Probe"
    Show-DiagUsage
    exit 2
}

$build = Resolve-SpanTuiBuildDirectory -Root $root
$app = Join-Path $build 'SpanTui-test.exe'
Require-File $app 'SpanTui-test executable (run dev.bat build first)'

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $build "diag\$Probe-$stamp"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# Probe args arrive verbatim after `dev diag <probe>`; normalize GNU-style
# --name to PowerShell -name so probes declare standard param blocks.
$normalized = @()
foreach ($arg in @($ProbeArgs)) {
    if ([string]::IsNullOrEmpty($arg)) { continue }
    if ($arg -match '^--([a-zA-Z].*)$') { $normalized += '-' + $Matches[1] } else { $normalized += $arg }
}

# Run the probe in a child host: its `exit` becomes a real process exit code
# the worker can propagate after printing the artifact location.
$hostExe = (Get-Process -Id $PID).Path
& $hostExe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $probeScript `
    -Root $root -BuildDir $build -AppPath $app -OutDir $outDir @normalized
$code = $LASTEXITCODE
Write-Host ''
Write-Host "diag artifacts: $outDir"
exit $code
