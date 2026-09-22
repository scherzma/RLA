param([ValidatePattern('^[A-Za-z0-9_-]+$')][string]$OutputFolder = 'RawInputProbe')
$ErrorActionPreference = 'Stop'
$repoPath = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsPath = & $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools were not found.' }
$devCmdPath = Join-Path $vsPath 'Common7/Tools/VsDevCmd.bat'
Push-Location $repoPath
try {
    New-Item -ItemType Directory -Force "x64/$OutputFolder" | Out-Null
    $compileCommand = 'cl /nologo /std:c++20 /EHsc /W4 /O2 /MD /DUNICODE /D_UNICODE /Ivcpkg_installed/x64-windows/include tools/RawInputProbe/main.cpp /Fo:x64/RawInputProbe/ /Fe:x64/RawInputProbe/RawInputProbe.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib'
    $compileCommand = $compileCommand.Replace('x64/RawInputProbe/', "x64/$OutputFolder/") + ' hid.lib shell32.lib'
    & $env:ComSpec /d /c "call `"$devCmdPath`" -arch=x64 -no_logo && $compileCommand"
    if ($LASTEXITCODE -ne 0) { throw 'Probe build failed.' }
    $test = Start-Process "./x64/$OutputFolder/RawInputProbe.exe" -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
    if ($test.ExitCode -ne 0) { throw 'Probe counter checks failed.' }
    $test = Start-Process "./x64/$OutputFolder/RawInputProbe.exe" -ArgumentList '--report-data --verify-report-data ' -WindowStyle Hidden -Wait -PassThru
    if ($test.ExitCode -ne 0) { throw 'Report-mode argument check failed.' }
    Write-Host 'Probe build and counter checks passed.'
} finally { Pop-Location }
