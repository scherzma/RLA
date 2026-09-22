# Independent Raw Input probe

Build: `./tools/RawInputProbe/build.ps1`. Output: `x64/RawInputProbe/RawInputProbe.exe`.

1. Close RLA and other mouse recorders.
2. Open the probe. Click **Assign A**, wait half a second, then move only A. Repeat for B.
3. Click **Start 18-second test**. Follow the instructions: prepare, move A, move both, move A.
4. Keep the window in the foreground. Esc or focus loss stops the test early.

Results save to `%LOCALAPPDATA%/RLA/RawInputProbe/`. The window shows the stage rates and file path. Keep moving throughout each stage. Incomplete tests are marked and should not be compared.

This program independently reads each `WM_INPUT` report with `GetRawInputData`. It does not link RLA code, use buffered reads, draw graphs, or trace USB events. It uses foreground registration with `RIDEV_NOLEGACY` during capture, a confined cursor, and the normal thread priority. RLA uses a separate high-priority capture thread and combined standard/buffered reads. Thus this comparison tests several capture-path differences; it does not isolate a single cause.

Counts are stored in fixed 10 ms bins. No files are written during capture. Stage rates use 4–7.5, 9–12.5, and 14–17.5 seconds, avoiding transitions. JSON includes total mouse reports, reports with nonzero relative movement, and reports with the `MOUSE_MOVE_NOCOALESCE` flag. Rates use movement reports, like RLA. Absolute input is counted separately and excluded. Times measure application receipt, not device polling. This test does not measure latency.

The build checks bin boundaries, movement filtering, and rate calculation. Physical capture performance still needs the two-mouse test.

References:
- [Microsoft: standard Raw Input processing](https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input)
- [Microsoft: RAWMOUSE fields](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawmouse)
- [MousePlotter Windows recorder](https://github.com/XBAB-Tech/MousePlotter/blob/main/windows_gui/log.c): uses standard reads and optional USB tracing, selecting one mouse. This probe is independently written and does not include MousePlotter code.

## USB trace comparison

Close other recorders, then run `tools/RawInputProbe/TraceUsb.cmd`. Accept the Windows administrator prompt. The helper starts USB tracing and opens the probe. Assign A and B and run one test. It stops tracing when that probe saves a result, exits, or after 120 seconds. It leaves the probe window open.

Files are saved under `%LOCALAPPDATA%/RLA/RawInputProbe/USB-<date>-<pid>/`. The helper copies the matching probe JSON beside the ETL. It uses a unique trace session, 256 MiB circular file limit, and only the USBXHCI, UCX, and USBHUB3 providers. `Default,HeadersBusTrace` records transfer metadata without USB data payloads. Other USB devices can appear in the metadata. Check event loss and endpoint identity before interpreting counts. The trace can add overhead; compare its Raw Input counts with the untraced baseline. Stage alignment from JSON file time is approximate, suitable only for broad stage comparisons.

The helper stops and deletes its own trace session in `finally`. If the helper is forcibly terminated, use an administrator terminal to stop and delete the `RLA-USB-Probe-<pid>` session shown by `logman query`. This does not remove saved ETL files.

See [Microsoft's USB capture instructions and keyword definitions](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/how-to-capture-a-usb-event-trace).

## Compare movement bytes

Build with `./tools/RawInputProbe/build.ps1 -OutputFolder RawInputReportProbe2`, then open `CompareReports.cmd`. This opens the separate report recorder, titled **USB report comparison**. The helper requires a version-2 startup handshake and rejects results without individual samples or a clock anchor. Use the same A-only, both, A-only sequence. Leave the mouse connections and DPI settings unchanged.

This mode enables `PartialDataBusTrace`, which records USB payload bytes, including possible traffic from other USB devices. Do not type while the test window is open. The files stay local. The original `TraceUsb.cmd` continues to capture headers only.

The probe stores up to 600,000 individual reports in preallocated memory, including zero movement, with QPC timestamps, device index, signed X/Y counts and flags. It saves a UTC/QPC clock anchor for alignment, HID value capabilities for each assigned mouse, and an explicit overflow count. Files are written after recording stops. HID capability reads use a zero-access device handle and do not change device settings. `--hid-info` writes a read-only capability inventory to the results directory for troubleshooting.

Analysis must decode the correct HID report format, exclude other endpoints and zero-movement USB reports, check trace loss and sample overflow, and compare signed movement over short windows as well as matched report sequences. Similar total counts alone do not prove report combining. An absent `MOUSE_MOVE_NOCOALESCE` flag does not prove that a particular event was combined.

The 2026-09-22 partial-data trace contained only transfer headers for the mouse completions. Enabling the payload keyword does not guarantee that usable USB report bytes are present. Check the ETL contents before attempting a byte comparison or requesting another recording.
