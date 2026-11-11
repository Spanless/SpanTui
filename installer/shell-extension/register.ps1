[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackagePath,
    [Parameter(Mandatory = $true)][string]$ExternalLocation,
    [Parameter(Mandatory = $true)][string]$CertificatePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([Environment]::OSVersion.Version.Build -lt 22000) {
    exit 0
}

$package = (Resolve-Path -LiteralPath $PackagePath).Path
$external = (Resolve-Path -LiteralPath $ExternalLocation).Path
$certificateFile = (Resolve-Path -LiteralPath $CertificatePath).Path
$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificateFile)

if ($certificate.Subject -ne 'CN=Span') {
    throw "Unexpected SpanTui identity certificate subject: $($certificate.Subject)"
}

$signature = Get-AuthenticodeSignature -LiteralPath $package
$rejectedStatuses = @(
    [System.Management.Automation.SignatureStatus]::HashMismatch,
    [System.Management.Automation.SignatureStatus]::NotSigned
)
$now = [DateTime]::Now
if (-not $signature.SignerCertificate -or
    $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint -or
    $signature.Status -in $rejectedStatuses -or
    $certificate.NotBefore -gt $now -or
    $certificate.NotAfter -le $now) {
    throw "SpanTui identity package signature validation failed: $($signature.StatusMessage)"
}

Get-AppxPackage -Name 'Span.SpanTui' -ErrorAction SilentlyContinue |
    Remove-AppxPackage -ErrorAction Stop
Add-AppxPackage -Path $package -ExternalLocation $external -ErrorAction Stop

$registered = Get-AppxPackage -Name 'Span.SpanTui' -ErrorAction SilentlyContinue
if (-not $registered) {
    throw 'SpanTui identity package registration did not persist.'
}
