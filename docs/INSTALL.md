# Install RLA

RLA is a Windows x64 desktop app. Use Windows 10 or 11 with DirectX 11 graphics support.

1. Open [GitHub Releases](https://github.com/scherzma/RLA/releases/latest).
2. Download **RLA-1.0.0-windows-x64.zip** under **Assets**. The automatic source-code archives are not the app.
3. Extract the ZIP into a folder where you have write access.
4. Open **RLA.exe**. No Visual Studio, Python, vcpkg, or separate C++ runtime installation is needed for the portable release.

The release also provides **RLA.exe** directly. The ZIP includes the installation notes and third-party license notices. The binary is not code-signed. Windows can show a publisher warning; verify that the file came from this repository. SHA-256 hashes are provided in the release.

## First recording

1. Click **Assign** beside A, then move only the reference mouse. Assign B the same way.
2. Use **Mouse settings** beside each mouse to select or create its name and connection. Link wired and wireless connections to the same name.
3. Click **Start Recording**. Move both mice together through several smooth back-and-forth cycles. Start Recording opens the graph.
4. Click or press **Esc** to stop raw capture. RLA saves the completed recording automatically.
5. Click **Analyze latency**, or let automatic analysis run for linked mice. The result names the faster mouse. **Recorded rate** shows the Hz stored for A and B.

Use one RLA instance at a time. Close input-sharing software when comparing mice. In local tests, active Synergy reduced the combined observed event rate; closing it restored the rate.

## Update or remove

Close RLA before replacing its executable. Extract a new release into a new folder or replace the old app files. Your recordings and library are stored separately in `%LOCALAPPDATA%\RLA`.

To remove the app, delete its extracted folder. To remove saved data too, back up anything you need and then delete `%LOCALAPPDATA%\RLA`. **Autosaves > Clear autosaves** removes only automatic recordings. Library Delete buttons remove selected entries and their related results, but keep recording files.

## If it does not start

- Extract the ZIP before opening the executable.
- Use the Windows x64 build. Linux and macOS are not supported.
- Check that your graphics driver supports DirectX 11.
- If the app stops responding, include the relevant file from `%LOCALAPPDATA%\RLA\Diagnostics` when reporting the issue. Saved recordings can help reproduce graph problems.
