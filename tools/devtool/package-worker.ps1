# `dev package`: Release build of SpanTui and the Explorer command DLL, deployed into a portable tree, plus the signed shell identity package and
# the Inno Setup installer. Everything is assembled in <output>.staging and
# replaces <output> only after every step passed.
[CmdletBinding()]
param(
    [switch]$Clean,
    # Portable tree only: no shell identity package, no installer.
    [switch]$PortableOnly,
    [switch]$ForceConfigure,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\toolchain.ps1')

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path (Resolve-SpanTuiBuildDirectory -Root $root) 'release'
$version = Get-SpanTuiVersion (Join-Path $root 'CMakeLists.txt')
$output = Resolve-SpanTuiReleaseDirectory -Root $root -Version $version
$staging = "$output.staging"
$portable = Join-Path $staging 'SpanTui-portable'
$installerScript = Join-Path $root 'installer\SpanTui.iss'
$shellSource = Join-Path $root 'installer\shell-extension'
# Built as SpanTui-pre so it never collides with a running SpanTui-test or the
# installed SpanTui; it ships as SpanTui.exe.
$preImage = Join-Path $build 'SpanTui-pre.exe'
$watch = [System.Diagnostics.Stopwatch]::StartNew()

$toolchain = Resolve-SpanTuiToolchain -RequireWindeployQt
Use-SpanTuiToolchainPath $toolchain

function Find-PackagingTool([string]$Name, [string[]]$Candidates) {
    $found = Get-FirstExistingPath $Candidates
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $found -and $command) { $found = $command.Source }
    if (-not $found) { Fail "$Name was not found; see docs/workflow/build.md, or use dev package --quick (portable only)." }
    return $found
}

# Tools of the full package are resolved before the long build.
if (-not $PortableOnly) {
    $iscc = Find-PackagingTool 'ISCC.exe' @($env:SPANTUI_ISCC,
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'), (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'))
    $sdkBins = @(Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+(\.\d+){3}$' } | Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64' })
    $makeAppx = Find-PackagingTool 'makeappx.exe' @($sdkBins | ForEach-Object { Join-Path $_ 'makeappx.exe' })
    $signTool = Find-PackagingTool 'signtool.exe' @($sdkBins | ForEach-Object { Join-Path $_ 'signtool.exe' })
    # tools\create-shell-signing-certificate.ps1 creates it with this friendly name.
    $thumbprint = "$env:SPANTUI_MSIX_CERT_THUMBPRINT".Replace(' ', '').ToUpperInvariant()
    $certificates = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object {
        $_.Subject -eq 'CN=Span' -and $_.HasPrivateKey -and $_.NotAfter -gt [DateTime]::Now.AddDays(30) -and
        $(if ($thumbprint) { $_.Thumbprint -eq $thumbprint } else { $_.FriendlyName -eq 'SpanTui Shell Identity Signing' })
    })
    if ($certificates.Count -ne 1) {
        Fail 'Exactly one usable CN=Span code-signing certificate is required for the shell identity package. Run tools\create-shell-signing-certificate.ps1 once, or set SPANTUI_MSIX_CERT_THUMBPRINT.'
    }
    $certificate = $certificates[0]
}

if ($Clean -and (Test-Path -LiteralPath $build)) {
    Write-Host "Removing release build directory: $build"
    Remove-Item -LiteralPath $build -Recurse -Force
}
Stop-SpanTuiTestInstances -ExecutablePath $preImage
Initialize-SpanTuiBuildTree -Toolchain $toolchain -SourceDir $root -BuildDir $build -BuildType 'Release' `
    -ExtraArgs @('-DSPANTUI_OUTPUT_NAME=SpanTui-pre', '-DSPANTUI_INSTALLED_IMAGE=ON') -Force:$ForceConfigure
Invoke-SpanTuiTimedStep 'Release build' {
    Invoke-SpanTuiBuild -Toolchain $toolchain -BuildDir $build -Target 'spantui_brand_icon_refresh' -Jobs $Jobs
    Invoke-SpanTuiBuild -Toolchain $toolchain -BuildDir $build `
        -Target @('SpanTui', 'SpanTui-shell-extension') -Jobs $Jobs
}
Invoke-SpanTuiTimedStep 'Release image smoke' {
    # Isolated: the smoke must never open the live workspace database.
    $smokeArgs = Format-SpanTuiAppArgs (@('--headless', '--isolated-workspace') + (Get-SpanTuiDevDataArgs -Root $root))
    $result = Test-SpanTuiStaysAlive -ExecutablePath $preImage -Arguments $smokeArgs -Hidden
    if (-not $result.Alive) { Fail "Release image exited early with code $($result.ExitCode): $preImage" }
}

if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
try {
    Invoke-SpanTuiTimedStep 'Portable tree' {
        $licenses = Join-Path $portable 'licenses'
        New-Item -ItemType Directory -Path (Join-Path $licenses 'Qt'), (Join-Path $portable 'shell'), (Join-Path $portable 'qml') -Force | Out-Null
        Copy-Item -LiteralPath $preImage -Destination (Join-Path $portable 'SpanTui.exe')
        Copy-Item -LiteralPath (Join-Path $build 'SpanTui-shell-extension.dll') -Destination $portable
        # Bundled ConPTY: conpty.dll starts the OpenConsole.exe beside it (docs/legal/conpty.md).
        Copy-Item -Path (Join-Path $root 'src\terminal\vendor\conpty\x64\*') -Destination $portable
        Copy-Item -Path (Join-Path $shellSource '*.ps1') -Destination (Join-Path $portable 'shell')
        if (Test-Path -LiteralPath (Join-Path $build 'SpanTui') -PathType Container) {
            Copy-Item -LiteralPath (Join-Path $build 'SpanTui') -Destination (Join-Path $portable 'qml\SpanTui') -Recurse
        }
        # Portable mode marker (ProductPaths); the installer excludes it.
        @('SpanTui keeps all of its data in the data folder next to SpanTui.exe',
          'while this file exists. Delete this file to use %LOCALAPPDATA%\SpanTui instead.') |
            Set-Content -LiteralPath (Join-Path $portable 'portable.txt') -Encoding ascii

        # Notices and license texts (docs/legal/third-party-notices.md).
        Copy-Item -LiteralPath (Join-Path $root 'docs\legal\third-party-notices.md') -Destination (Join-Path $portable 'THIRD_PARTY_NOTICES.md')
        Copy-Item -Path (Join-Path $root 'docs\legal\licenses\*'), (Join-Path $root 'assets\fonts\SpanFonts-LICENSE-*.txt') -Destination $licenses
        Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $licenses 'SpanTui-LICENSE.txt')
        Copy-Item -LiteralPath (Join-Path $root 'src\terminal\vendor\libghostty-vt\LICENSE') -Destination (Join-Path $licenses 'libghostty-vt-LICENSE.txt')
        Copy-Item -LiteralPath (Join-Path $root 'src\terminal\vendor\conpty\LICENSE') -Destination (Join-Path $licenses 'ConPTY-LICENSE.txt')
        # SBOMs of the shipped Qt modules list Qt's bundled third-party code.
        foreach ($module in @('qtbase', 'qtdeclarative', 'qtsvg', 'qtimageformats', 'qttranslations')) {
            $sbom = @(Get-ChildItem -LiteralPath (Join-Path $toolchain.QtRoot 'sbom') -Filter "$module-*.spdx" -File |
                Where-Object { $_.Name -notlike '*.source.spdx' })
            if ($sbom.Count -ne 1) { Fail "Expected one Qt SBOM for $module; found $($sbom.Count)." }
            Copy-Item -LiteralPath $sbom[0].FullName -Destination (Join-Path $licenses 'Qt')
        }

        # Qt Virtual Keyboard is GPL-3.0 only in open-source Qt and unused. Its
        # input-context plugin is skipped, which keeps its library out too; no
        # --no-virtualkeyboard here, since windeployqt only accepts that flag when
        # the module is installed. Only the SQLite driver ships.
        $filters = @('--release', '--no-opengl-sw',
            '--skip-plugin-types', 'qmltooling,platforminputcontexts',
            '--exclude-plugins', 'qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql')
        & $toolchain.WindeployQt @filters --compiler-runtime --qmldir (Join-Path $root 'qml') (Join-Path $portable 'SpanTui.exe')
        if ($LASTEXITCODE -ne 0) { Fail 'windeployqt failed for SpanTui.' }
        $gplOnly = @(Get-ChildItem -LiteralPath $portable -Recurse -File -Filter '*VirtualKeyboard*')
        if ($gplOnly.Count -gt 0) { Fail ('GPL-only Qt Virtual Keyboard files reached the package: ' + ($gplOnly.FullName -join ', ')) }

        # Build stamp of the installed image: the update check compares a
        # package's builtAtUtc with it (docs/services/update/updater.md).
        $stamp = Get-Content -LiteralPath (Join-Path $build 'spantui-build.json') -Raw | ConvertFrom-Json
        [ordered]@{
            version = $stamp.version
            builtAtUtc = $stamp.builtAtUtc
            exe = 'SpanTui.exe'
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $portable 'spantui-build.json') -Encoding utf8
    }

    $artifacts = @('SpanTui-portable/SpanTui.exe', 'SpanTui-portable/SpanTui-shell-extension.dll')
    if (-not $PortableOnly) {
        Invoke-SpanTuiTimedStep 'Shell identity package' {
            # Sparse package that gives the Explorer command DLL its identity on
            # Windows 11 (docs/services/platform/windows/explorer-integration.md).
            $layout = Join-Path $staging 'shell-identity'
            New-Item -ItemType Directory -Path (Join-Path $layout 'Assets') -Force | Out-Null
            (Get-Content -LiteralPath (Join-Path $shellSource 'AppxManifest.xml.in') -Raw).Replace('@VERSION@', "$version.0") |
                Set-Content -LiteralPath (Join-Path $layout 'AppxManifest.xml') -Encoding utf8
            foreach ($asset in @('StoreLogo.png', 'Square44x44Logo.png', 'Square150x150Logo.png')) {
                Copy-Item -LiteralPath (Join-Path $root "assets\brand\$asset") -Destination (Join-Path $layout 'Assets')
            }
            $msix = Join-Path $portable 'shell\SpanTui-shell-identity.msix'
            & $makeAppx pack /o /nv /d $layout /p $msix
            if ($LASTEXITCODE -ne 0) { Fail 'MakeAppx failed for the shell identity package.' }
            & $signTool sign /sha1 $certificate.Thumbprint /fd SHA256 $msix
            if ($LASTEXITCODE -ne 0) { Fail 'SignTool failed for the shell identity package.' }
            Export-Certificate -Cert $certificate -FilePath (Join-Path $portable 'shell\SpanTui-shell-identity.cer') -Force | Out-Null
            Remove-Item -LiteralPath $layout -Recurse -Force
        }
        Invoke-SpanTuiTimedStep 'Inno Setup installer' {
            $selfSigned = [int]($certificate.Subject -eq $certificate.Issuer)
            & $iscc '/Qp' "/DMyAppVersion=$version" "/DMyAppSourceDir=$portable" "/DMyOutputDir=$staging" `
                "/DMyShellCertificateSelfSigned=$selfSigned" $installerScript
            if ($LASTEXITCODE -ne 0) { Fail 'Inno Setup compilation failed.' }
        }
        $artifacts += @('SpanTui-portable/shell/SpanTui-shell-identity.msix',
            'SpanTui-portable/shell/SpanTui-shell-identity.cer', "SpanTui-$version-Setup.exe")
    }

    Invoke-SpanTuiTimedStep 'Portable archive' {
        # The portable download of a release; installed copies update from the
        # Setup (docs/services/update/updater.md). Windows' own bsdtar writes
        # zip; a GNU tar earlier on PATH would not.
        $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
        & $tar -a -c -f (Join-Path $staging "SpanTui-$version-portable.zip") -C $staging 'SpanTui-portable'
        if ($LASTEXITCODE -ne 0) { Fail "tar failed to create the portable archive with exit code $LASTEXITCODE." }
    }
    $artifacts += "SpanTui-$version-portable.zip"

    # A local update source compares builtAtUtc with the installed build stamp.
    $builtAtUtc = (Get-Content -LiteralPath (Join-Path $portable 'spantui-build.json') -Raw | ConvertFrom-Json).builtAtUtc
    [ordered]@{
        name = 'SpanTui'
        version = $version
        builtAtUtc = $builtAtUtc
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        artifacts = @($artifacts | ForEach-Object {
            $path = Join-Path $staging $_
            [ordered]@{ file = $_; size = (Get-Item -LiteralPath $path).Length; sha256 = Get-Sha256 $path }
        })
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $staging 'release-manifest.json') -Encoding utf8

    if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output -Recurse -Force }
    Move-Item -LiteralPath $staging -Destination $output
} finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ("Package complete in {0:n1}s: {1}" -f $watch.Elapsed.TotalSeconds, $output) -ForegroundColor Green
foreach ($artifact in $artifacts) {
    $item = Get-Item -LiteralPath (Join-Path $output $artifact)
    Write-Host ("  {0} ({1:N0} bytes)" -f $item.FullName, $item.Length)
}
