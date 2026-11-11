[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$friendlyName = 'SpanTui Shell Identity Signing'
$existing = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object {
        $_.FriendlyName -eq $friendlyName -and
        $_.Subject -eq 'CN=Span' -and
        $_.NotAfter -gt [DateTime]::UtcNow.AddDays(30) -and
        $_.HasPrivateKey
    })

if ($existing.Count -gt 1) {
    throw "Multiple usable '$friendlyName' certificates exist; remove the stale duplicate first."
}
if ($existing.Count -eq 1) {
    Write-Host "Existing SpanTui shell signing certificate: $($existing[0].Thumbprint)"
    exit 0
}

$certificate = New-SelfSignedCertificate `
    -Type Custom `
    -Subject 'CN=Span' `
    -FriendlyName $friendlyName `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -KeyUsage DigitalSignature `
    -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') `
    -KeyAlgorithm RSA `
    -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -KeyExportPolicy NonExportable `
    -NotAfter ([DateTime]::UtcNow.AddYears(5))

Write-Host "Created SpanTui shell signing certificate: $($certificate.Thumbprint)"
