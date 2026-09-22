Run `./tests/run.ps1` from PowerShell. The script needs Visual Studio C++ tools and the installed x64 vcpkg libraries. It builds a console test executable in `x64/Regression`.

The checks cover imported plot data, saved timer frequency, movement magnitude, recording boundaries, assignment cancellation, rate resets, JSON validation, CSV output, and loading the existing repository session.

Scale checks cover delay, different event rates (including 125 Hz and 8 kHz), independent movement, collision outliers, conflicting scale estimates, insufficient data, and clearing stale settings. Timing checks cover exact 8 kHz intervals, moving means, idle windows, duplicate timestamps, and interval statistics. All three plot modes generate ImGui/ImPlot frames without a GPU. These checks do not test real mouse timing or visible window interaction.

Binning checks reproduce whole-report boundary spikes with a steady signal and timestamp jitter. They compare the error with time weighting, verify signed count conservation at fractional bin widths, and cover clock drift, duplicate timestamps, long idle gaps, and invalid parameters. Plot frames exercise both weighting settings.

Scale regression coverage also includes a small axis rotation in the dominant matching movement, with a minority of aligned sections at a different scale. This prevents vector-based selection from choosing the wrong amplitude for the magnitude plot.

After building, run `./x64/Regression/Regression.exe <session.json> <manual-scale>` for a targeted check of a local recording. This prints the automatic scale and accepted section count, then requires agreement with the supplied manual scale within 1%. It does not copy the recording into the repository.

Stop/restart checks use two 2 kHz movement segments separated by a one-second pause. They verify a break in the mean curve, immediate mean recovery, preserved raw gap statistics, zero window rates during idle time, and the configurable gap threshold. The UI tests inspect actual ImPlot axis ranges after showing/hiding gaps and resetting an excessively wide view.

View tests verify that manual panning below zero remains intact, hiding settings and summary increases graph height, and both single-graph modes use the available height.

References used for the fixes:

- [Microsoft: QueryPerformanceFrequency](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency). The counter frequency is fixed at system boot. Imported recordings must use their stored frequency.
- [nlohmann JSON: Number handling](https://json.nlohmann.me/features/types/number_handling/). Numeric conversions can change the value. The loader checks integer types and ranges before conversion.

Buffered-input checks simulate 16,000 separate reports from two mice. They check report order, device identity, unchanged counts, shared batch timestamps, buffer growth, empty queues, invalid packets, read failures, queue overflow, and stopped input. A real message-only window checks registration and thread shutdown. JSON round trips preserve optional capture diagnostics; negative counters are rejected. These tests do not emulate USB hardware or prove real 8 kHz performance.

UI message-loop checks simulate continuously available client and title-bar cursor movement. They require a bounded drain, delivery of button/key/wheel/resize messages, continued movement on the next frame, and immediate handling of quit. Cursor confinement requires a foreground RLA window; headless recording checks must leave the system cursor unrestricted. Actual focus changes and cursor confinement require a manual window check.

Raw-capture checks verify the mouse-only `RIDEV_NOLEGACY` registration, restoration of the previous registration flags, explicit registration failure reporting, one-shot stop clicks with zero movement, Esc and focus-loss stopping, and capture-mode metadata round trips. Failure tests use a fake API. A native Windows check also enables and restores the mode and queries the actual registered flags. Physical capture performance and interactive controls still need a new recording.
