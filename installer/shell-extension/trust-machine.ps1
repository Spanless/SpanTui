[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Install', 'Remove', 'Check')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$CertificatePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$certificateFile = (Resolve-Path -LiteralPath $CertificatePath).Path
$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificateFile)
if ($certificate.Subject -ne 'CN=Span' -or $certificate.Subject -ne $certificate.Issuer) {
    throw "Unexpected SpanTui self-signed certificate identity: $($certificate.Subject)"
}

$basicConstraints = @($certificate.Extensions |
    Where-Object { $_ -is [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension] })
if ($basicConstraints.Count -gt 0 -and $basicConstraints[0].CertificateAuthority) {
    throw 'SpanTui refuses to trust a certificate-authority certificate.'
}

$codeSigningOid = '1.3.6.1.5.5.7.3.3'
$hasCodeSigningUsage = @($certificate.Extensions |
    Where-Object { $_ -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] } |
    ForEach-Object { $_.EnhancedKeyUsages } |
    Where-Object { $_.Value -eq $codeSigningOid }).Count -gt 0
if (-not $hasCodeSigningUsage) {
    throw 'SpanTui identity certificate is not restricted to code signing.'
}

$trustedPath = "Cert:\LocalMachine\TrustedPeople\$($certificate.Thumbprint)"
# Check only reads the machine store, which needs no elevation; it exits 0
# when the certificate is already trusted (an upgrade skips the UAC prompt).
if ($Mode -eq 'Check') {
    if (Test-Path -LiteralPath $trustedPath) { exit 0 }
    exit 1
}

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Machine certificate trust requires an elevated administrator token.'
}

if ($Mode -eq 'Install') {
    if (-not (Test-Path -LiteralPath $trustedPath)) {
        Import-Certificate -FilePath $certificateFile `
            -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
    }
    if (-not (Test-Path -LiteralPath $trustedPath)) {
        throw 'SpanTui machine certificate trust did not persist.'
    }
} elseif (Test-Path -LiteralPath $trustedPath) {
    Remove-Item -LiteralPath $trustedPath -Force
}
