# Build from source

## Command line: no Visual Studio IDE

Install these tools once:

- [Git for Windows](https://git-scm.com/download/win).
- [Microsoft C++ Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022). Select **Desktop development with C++**, the MSVC x64 compiler, a Windows SDK, and **C++ CMake tools for Windows**.

The Build Tools installer is sufficient; the Visual Studio editor is not required. See [Microsoft's command-line guide](https://learn.microsoft.com/en-us/cpp/build/building-on-the-command-line).

Open PowerShell and run:

```powershell
git clone https://github.com/scherzma/RLA.git
cd RLA
git clone https://github.com/microsoft/vcpkg.git ..\vcpkg
..\vcpkg\bootstrap-vcpkg.bat
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -VcpkgRoot ..\vcpkg -Test -Package
.\out\cmake-release\RLA.exe
```

The script finds the compiler, CMake, and Ninja, then uses your vcpkg checkout. It builds Release/x64 with the static C++ runtime. The first build downloads and compiles dependencies. Internet access is required. It does not install tools or change system environment variables.

`-Test` runs the regression checks. `-Package` creates `out/cmake-release/RLA-1.0.0-windows-x64.zip`. Omit either switch if it is not needed. Do not run tests while taking a measurement.

You can also set `VCPKG_ROOT` for the current shell. Dependencies and their registry baseline are declared in `vcpkg.json` and `vcpkg-configuration.json`. CMake uses the [official vcpkg toolchain integration](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration).

## Direct CMake commands

From an **x64 Native Tools Command Prompt**, with CMake and Ninja on PATH:

```bat
cmake -S . -B out/cmake-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/src/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DBUILD_TESTING=ON
cmake --build out/cmake-release --parallel
ctest --test-dir out/cmake-release --output-on-failure
cmake --build out/cmake-release --target package
```

MSVC is the tested compiler. This is a Windows application; CMake does not make it run on Linux or macOS. MinGW is not currently a tested build path.

## Visual Studio

You can still open `RLA.slnx` in Visual Studio with the C++ desktop workload and build **Release / x64**. That project currently selects toolset `v145`. Use the CMake route above with other supported MSVC installations. The existing `tests/run.ps1` script remains available for the MSBuild workflow.

## What the tests cover

Tests cover capture queues, session persistence, scale and latency fits, automatic Hz detection, ranking, deletion, and UI frame generation. They do not establish physical mouse timing accuracy. See [test details](../tests/README.md).
