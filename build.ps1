param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'out/cmake-release'),
    [switch]$Test,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath)) { throw 'Install Microsoft C++ Build Tools with Desktop development with C++ first. See docs/BUILD.md.' }
$vsPath = & $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'The MSVC x64 compiler is missing. See docs/BUILD.md.' }
$devCmd = Join-Path $vsPath 'Common7/Tools/VsDevCmd.bat'
# Import the tool environment into this process only. Do not change system settings.
$toolEnvironment = & $env:ComSpec /d /c "call `"$devCmd`" -arch=x64 -host_arch=x64 -no_logo >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the compiler environment.' }
foreach ($line in $toolEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$cmakeRoot = Join-Path $vsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake'
$cmake = Join-Path $cmakeRoot 'CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $command) { throw 'Install C++ CMake tools for Windows in the Build Tools installer.' }
    $cmake = $command.Source
}
$ninjaDirectory = Join-Path $cmakeRoot 'Ninja'
if (Test-Path -LiteralPath $ninjaDirectory) { $env:PATH = "$ninjaDirectory;$env:PATH" }
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { throw 'Ninja is missing. Install C++ CMake tools for Windows.' }
if (-not $VcpkgRoot) { throw 'Set VCPKG_ROOT or pass -VcpkgRoot. See docs/BUILD.md for vcpkg setup.' }
$toolchain = Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'
if (-not (Test-Path -LiteralPath $toolchain)) { throw 'Pass -VcpkgRoot with the path to vcpkg. See docs/BUILD.md.' }
$toolchain = (Resolve-Path -LiteralPath $toolchain).Path
$testing = if ($Test) { 'ON' } else { 'OFF' }
& $cmake -S $PSScriptRoot -B $BuildDirectory -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_TOOLCHAIN_FILE=$toolchain" '-DVCPKG_TARGET_TRIPLET=x64-windows-static' "-DBUILD_TESTING=$testing"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $BuildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Test) {
    & (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir $BuildDirectory --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Regression checks failed.' }
}
if ($Package) {
    & $cmake --build $BuildDirectory --target package
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
}
Write-Host "Built: $(Join-Path $BuildDirectory 'RLA.exe')"
