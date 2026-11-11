# tools/update-conpty.ps1 — refresh the bundled ConPTY (conpty.dll +
# OpenConsole.exe) from the Microsoft.Windows.Console.ConPTY NuGet package.
#
# Downloads into a fresh directory under .dev\tmp\, requires a valid Microsoft
# Authenticode signature on both files, copies the x64 runtime into
# src/terminal/vendor/conpty/x64/ and prints the hashes to record in
# docs/legal/conpty.md. Needs network access: run it only when updating.
#
# Usage:  tools\update-conpty.ps1 -Version 1.25.260930003
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path $PSScriptRoot
$target = Join-Path $repoRoot 'src\terminal\vendor\conpty\x64'
$work = Join-Path $repoRoot ('.dev\tmp\conpty-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $id = 'microsoft.windows.console.conpty'
    $lower = $Version.ToLowerInvariant()
    $package = Join-Path $work "$id.$lower.nupkg"
    Invoke-WebRequest -UseBasicParsing -OutFile $package `
        -Uri "https://api.nuget.org/v3-flatcontainer/$id/$lower/$id.$lower.nupkg"

    $extracted = Join-Path $work 'package'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($package, $extracted)
    $files = [ordered]@{
        'conpty.dll'      = Join-Path $extracted 'runtimes\win-x64\native\conpty.dll'
        'OpenConsole.exe' = Join-Path $extracted 'build\native\runtimes\x64\OpenConsole.exe'
    }
    foreach ($name in $files.Keys) {
        $signature = Get-AuthenticodeSignature -LiteralPath $files[$name]
        if ($signature.Status -ne 'Valid' -or
            $signature.SignerCertificate.Subject -notlike 'CN=Microsoft Corporation,*') {
            throw "$name in $Version is not signed by Microsoft ($($signature.Status))."
        }
    }

    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($name in $files.Keys) {
        Copy-Item -LiteralPath $files[$name] -Destination (Join-Path $target $name) -Force
    }
    Write-Host ('Package {0}  sha256 {1}' -f $Version, (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash)
    foreach ($name in $files.Keys) {
        Write-Host ('{0}  sha256 {1}' -f $name,
            (Get-FileHash -LiteralPath (Join-Path $target $name) -Algorithm SHA256).Hash)
    }
    Write-Host 'Record the version and hashes in docs/legal/conpty.md.'
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
