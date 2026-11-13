# Creates (or reuses) the self-signed CN=Span code-signing certificate that
# signs the shell identity package (docs/workflow/release.md), and backs it up.
#
# The certificate lives in Cert:\CurrentUser\My with an exportable key, so the
# maintainer can package locally (dev package) and CI can release with a copy:
#   * -BackupDir writes SpanTui-shell-signing.pfx, its password file and the
#     public .cer to a folder outside the checkout (dev clean deletes .dev\);
#     keep that folder somewhere safe, e.g. next to the password manager.
#   * -Publish creates the GitHub environment `release`, admits only master to
#     it (deployment branch policy, so no pull request run can read its
#     secrets) and stores the PFX and password as its secrets
#     SHELL_SIGNING_PFX and SHELL_SIGNING_PFX_PASSWORD. Needs gh auth login
#     with admin rights on the repository.
#   * -Replace removes an existing certificate whose key cannot be exported
#     (created by an earlier version of this script) and creates a new one.
#     The installer trusts a certificate by thumbprint, so after a
#     replacement the next update of an installed copy asks for elevation
#     once to trust the new one; afterwards updates are silent again.
#
# Usage:  tools\create-shell-signing-certificate.ps1 -BackupDir <folder> [-Publish] [-Replace]
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BackupDir,
    [switch]$Publish,
    [switch]$Replace
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$friendlyName = 'SpanTui Shell Identity Signing'
$environment = 'release'

$backup = [System.IO.Path]::GetFullPath($BackupDir)
if ($backup.TrimEnd('\') -eq $root -or $backup.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "BackupDir must be outside the checkout ($root): a private key must never be committed or removed by dev clean."
}

function New-SigningCertificate {
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
        -KeyExportPolicy Exportable `
        -NotAfter ([DateTime]::UtcNow.AddYears(5))
    Write-Host "Created SpanTui shell signing certificate: $($certificate.Thumbprint)"
    return $certificate
}

# 24 random bytes; base64 keeps the password to ASCII without whitespace.
$bytes = New-Object byte[] 24
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$password = [Convert]::ToBase64String($bytes)
$securePassword = ConvertTo-SecureString -String $password -AsPlainText -Force

New-Item -ItemType Directory -Force -Path $backup | Out-Null
$pfx = Join-Path $backup 'SpanTui-shell-signing.pfx'
$cer = Join-Path $backup 'SpanTui-shell-signing.cer'
$passwordFile = Join-Path $backup 'SpanTui-shell-signing-password.txt'

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

$certificate = $null
if ($existing.Count -eq 1) {
    $certificate = $existing[0]
    try {
        Export-PfxCertificate -Cert $certificate -FilePath $pfx -Password $securePassword -Force | Out-Null
        Write-Host "Existing SpanTui shell signing certificate: $($certificate.Thumbprint)"
    } catch {
        if (-not $Replace) {
            throw "The key of certificate $($certificate.Thumbprint) cannot be exported, so CI cannot sign with it. Run again with -Replace to create an exportable one instead."
        }
        Write-Host "Removing certificate $($certificate.Thumbprint): its key cannot be exported."
        Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -DeleteKey
        $certificate = $null
    }
}
if (-not $certificate) {
    $certificate = New-SigningCertificate
    Export-PfxCertificate -Cert $certificate -FilePath $pfx -Password $securePassword -Force | Out-Null
}
Export-Certificate -Cert $certificate -FilePath $cer -Force | Out-Null
Set-Content -LiteralPath $passwordFile -Value $password -Encoding ascii
Write-Host "Backup written to ${backup}: $(Split-Path -Leaf $pfx), $(Split-Path -Leaf $passwordFile), $(Split-Path -Leaf $cer)"

if (-not $Publish) { exit 0 }

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'The GitHub CLI (gh) is required: https://cli.github.com, then gh auth login.' }
$origin = [string](& git -C $root remote get-url origin)
if ($origin -notmatch 'github\.com[:/]([^/]+/[^/]+?)(\.git)?/?$') { throw "origin is not a GitHub repository: $origin" }
$repo = $Matches[1]

# Pipeline input (a secret value) goes to gh's stdin, never its command line.
function Invoke-Gh {
    $output = $input | & gh @args
    if ($LASTEXITCODE -ne 0) { throw "gh $($args -join ' ') failed with exit code $LASTEXITCODE." }
    return $output
}

# Text piped to gh (JSON, secret values) must reach it without a byte order
# mark. Windows PowerShell writes a native command's stdin with the console's
# input encoding, so a profile that sets [Console]::InputEncoding to
# [Text.Encoding]::UTF8 prefixes one, which breaks the JSON and would end up
# inside the secrets. The console's encoding is put back afterwards.
$utf8 = New-Object System.Text.UTF8Encoding $false
$consoleInput = [Console]::InputEncoding
$OutputEncoding = $utf8
[Console]::InputEncoding = $utf8
try {
    # Only branches named by a custom policy may deploy to the environment;
    # the one policy is master.
    '{"deployment_branch_policy":{"protected_branches":false,"custom_branch_policies":true}}' |
        Invoke-Gh api --method PUT "repos/$repo/environments/$environment" --input - | Out-Null
    $policies = @(Invoke-Gh api "repos/$repo/environments/$environment/deployment-branch-policies" --jq '.branch_policies[].name')
    if ($policies -notcontains 'master') {
        Invoke-Gh api --method POST "repos/$repo/environments/$environment/deployment-branch-policies" -f name=master -f type=branch | Out-Null
    }
    $others = @($policies | Where-Object { $_ -and $_ -ne 'master' })
    if ($others.Count -gt 0) {
        throw "Environment $environment also admits $($others -join ', '); remove those deployment branch policies on GitHub, then run again."
    }

    [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($pfx)) |
        Invoke-Gh secret set SHELL_SIGNING_PFX --env $environment --repo $repo | Out-Null
    $password | Invoke-Gh secret set SHELL_SIGNING_PFX_PASSWORD --env $environment --repo $repo | Out-Null
} finally {
    [Console]::InputEncoding = $consoleInput
}
Write-Host "Environment $environment of $repo admits master only and holds the signing certificate $($certificate.Thumbprint)."
