# Builds and tests the terminal stress TUI with the SpanTui Qt/MinGW toolchain,
# then deploys Qt runtime DLLs next to the exe so it runs inside any terminal.
# Prints the exe path on the last line.
param(
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\toolchain.ps1')

# The tree lives in the product build tree (target\build, or the checkout's tree under SPANTUI_BUILD_ROOT).
$buildDir = Join-Path (Resolve-SpanTuiBuildDirectory -Root (Join-Path $PSScriptRoot '..\..')) 'terminal-stress-tui'

$toolchain = Resolve-SpanTuiToolchain -RequireWindeployQt
Use-SpanTuiToolchainPath $toolchain

Initialize-SpanTuiBuildTree -Toolchain $toolchain -SourceDir $PSScriptRoot -BuildDir $buildDir `
    -BuildType Release -ExtraArgs @('-DBUILD_TESTING=ON')

& $toolchain.CMake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { Fail 'terminal-stress-tui build failed.' }

if (-not $SkipTests) {
    & $toolchain.CTest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { Fail 'terminal-stress-tui tests failed.' }
}

$exe = Join-Path $buildDir 'terminal-stress-tui.exe'
& $toolchain.WindeployQt --no-translations --no-system-d3d-compiler --no-opengl-sw `
    --compiler-runtime --verbose 0 $exe
if ($LASTEXITCODE -ne 0) { Fail 'windeployqt failed for terminal-stress-tui.' }

Write-Output $exe
