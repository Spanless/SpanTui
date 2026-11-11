[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Get-AppxPackage -Name 'Span.SpanTui' -ErrorAction SilentlyContinue |
    Remove-AppxPackage -ErrorAction SilentlyContinue
