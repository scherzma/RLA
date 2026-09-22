$ErrorActionPreference = 'Stop'
$repoPath = Split-Path $PSScriptRoot -Parent
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsPath = & $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools were not found.' }
$devCmdPath = Join-Path $vsPath 'Common7/Tools/VsDevCmd.bat'
Push-Location $repoPath
try {
    New-Item -ItemType Directory -Force -Path 'x64/Regression' | Out-Null
    $compileCommand = 'cl /nologo /std:c++20 /EHsc /O2 /MD /DUNICODE /D_UNICODE /I. /Ivcpkg_installed/x64-windows/include tests/Regression.cpp src/App.cpp src/Analyzer.cpp src/Latency.cpp src/MouseLibrary.cpp src/MouseLibraryUI.cpp src/Autosave.cpp src/DataStore.cpp src/DeviceManager.cpp src/InputEngine.cpp src/Renderer.cpp /Fo:x64/Regression/ /Fe:x64/Regression/Regression.exe /link /LIBPATH:vcpkg_installed/x64-windows/lib imgui.lib implot.lib d3d11.lib dxgi.lib d3dcompiler.lib comdlg32.lib shell32.lib user32.lib gdi32.lib imm32.lib dwmapi.lib'
    & $env:ComSpec /d /c "call `"$devCmdPath`" -arch=x64 -no_logo && $compileCommand"
    if ($LASTEXITCODE -ne 0) { throw 'Regression build failed.' }
    & './x64/Regression/Regression.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Regression checks failed.' }
} finally {
    Pop-Location
}
