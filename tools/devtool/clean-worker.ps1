# `dev clean`: deletes the checkout's .dev\ (dev-run data root, test
# TEMP/APPDATA, tool work files, manual debugging output). Build trees and packages (target\ or
# the .env locations) stay; use `dev build --clean` / `dev package --clean`.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\toolchain.ps1')

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dev = Join-Path $root '.dev'
if (-not (Test-Path -LiteralPath $dev)) {
    Write-Host 'Nothing to clean: .dev\ does not exist.'
    Write-Host 'CLEAN_OK'
    return
}

# A running dev image holds files in .dev\data; stop this checkout's only.
$build = Resolve-SpanTuiBuildDirectory -Root $root
Stop-SpanTuiTestInstances -ExecutablePath (Join-Path $build 'SpanTui-test.exe')

Write-Host "Removing $dev"
Remove-Item -LiteralPath $dev -Recurse -Force -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath $dev) {
    $left = @(Get-ChildItem -LiteralPath $dev -Recurse -Force -File -ErrorAction SilentlyContinue)
    Fail ("{0} file(s) under .dev\ are still in use; close the program holding them and run dev clean again." -f $left.Count)
}
Write-Host 'CLEAN_OK'
