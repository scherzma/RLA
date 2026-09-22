# Relative Latency Analyzer

RLA records movement from two mice through Windows Raw Input. Use the plots to compare their movement and timing.

## Use

1. Assign the reference mouse as A and the test mouse as B.
2. Record both mice moving together. Include changes in speed or direction.
3. Stop recording. Select **Auto Scale B** to fit B's movement amplitude to A.
4. Select **Event timing / Hz** in **Plot Mode** to inspect intervals and event rates.

Auto Scale B checks direction and movement shape in separate sections. It excludes weak matches, idle sections, and conflicting scale estimates. It needs at least three matching sections. A failed fit leaves the current scale unchanged.

The fit uses common time bins, normally 2 ms. Slower event streams use larger bins. Each section spans at least 50 ms and eight bins. Direction checks reject unrelated movement. Magnitude correlation checks the shape shown in the movement plot. The scale for each section is the ratio of summed movement magnitudes; small differences between the mice's axes do not reduce this ratio. The fit checks offsets up to 10 ms in whole bins. These offsets only help find matching movement; no cross-mouse time shift is applied to the plots. The median scale from the largest consistent group is used. That group must contain at least two thirds of the accepted estimates and at least three sections. This is a heuristic, not a device calibration or a latency measurement. A scalar multiplier cannot correct an axis rotation in the X/Y plot.

Auto Scale B enables time-weighted binning so different event rates do not change the amplitude comparison. The multiplier applies to both movement magnitude and X/Y plots. Manual scaling remains available. Loading or starting a recording clears the previous scale. Saved events and CSV data retain their original values.

**Weight by time** estimates uniform movement between adjacent reports. It splits each report's X/Y counts across the bins that overlap that interval. This reduces spikes caused by whole reports crossing bin boundaries. The plots and scale fit use the same binning code. Signed counts are conserved. Binned magnitudes are counts per bin; raw magnitudes are counts per event, so their heights can differ.

The first report, duplicate timestamps, and reports after long gaps remain at arrival time. A long gap exceeds three times the median report interval or 50 ms. Movement is not spread across those gaps. Turn **Weight by time** off to inspect the original whole-event totals. Weighted bins are an estimate: arrival jitter can still affect them, and binning reduces time resolution. Use unbinned events and the timing view to inspect application read times.

The timing view includes:

- Raw interval points and a moving mean over the selected window. Long gaps are hidden from the interval plot by default.
- Event rates over a 10–500 ms window, including zero rates during idle periods.
- Optional per-interval rates, calculated as `1000 / interval_ms`.
- Event counts, median and 95th-percentile intervals, maximum gaps, long-gap counts, and counts of non-positive intervals. Interval statistics use positive intervals only and retain long gaps.

The timing view's **Long gap** setting defaults to 20 ms. Longer intervals break the mean curve and reset its averaging window. This prevents a pause from creating a ramp or distorting the mean after movement restarts. **Show long gaps** reveals the original large interval points. Long gaps can include pauses or delayed input; the threshold is a display choice, not a diagnosis. Rate calculations still include all elapsed time, so the rate falls to zero while no movement events arrive.

**Reset timing view** fits the plots to the recording with a small time margin. Loading or starting a recording resets the view. Both axes allow free panning beyond the data, including below zero. Turn off **Follow recording** to adjust the view freely during capture.

The timing toolbar offers **Both graphs**, **Intervals only**, and **Hz only**. A single graph uses the full available plot height. In the two-graph view, drag the divider to adjust their relative heights. **Settings** and **Summary** start hidden and can be opened when needed. **Device assignment** also collapses; recording controls remain available.

Timing curves use thick, light blue and light amber lines above smaller, faint raw points. Turn off **Raw points** for a clear view of the curves, or adjust **Point opacity** under **Settings**. These display controls do not change the timing calculations.

The two timing plots share their time axis. Statistics cover the full recording. The rate uses the number of events in `(time - window, time]`, divided by the window duration. Recordings shorter than the window use their available duration. Timing data refreshes at most ten times per second during recording.

The capture code stores movement events only. Timestamps are taken when the application processes Raw Input. Thus, the plots show observed movement-event arrival times, not hardware USB polling times. The session's stored counter frequency is used for imported recordings. See [Microsoft: RAWMOUSE](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawmouse), [Microsoft: QueryPerformanceCounter](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter), and the capture implementation in `src/InputEngine.cpp`.

Capture uses `GetRawInputBuffer` on a dedicated thread. The message loop leaves `WM_INPUT` messages queued for buffered reads and wakes when input arrives; it does not wait for a rendering frame or a fixed-rate timer. Each mouse report keeps its own device and movement counts. The reusable buffer grows if required. This follows [Microsoft's buffered input guidance](https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input).

Windows does not supply a device timestamp in `RAWINPUT`. All reports returned in one read therefore share the QPC time taken after that read. Zero intervals within a batch are expected; they are not measured zero-duration USB intervals. The window rate still counts each movement event. Buffered capture cannot recover original report times or split reports already combined upstream. Existing recordings remain unchanged.

**Summary** shows capture totals since application start: mouse reports, reports read in groups, largest batch, application queue drops, and API/packet errors. These totals include all received mice and activity outside the recording. They are saved as optional `capture` metadata and frozen when recording stops. Older JSON sessions remain supported. Use a new recording to check high-rate capture; synthetic regression checks cannot confirm physical USB performance.

**Keep cursor in window** is on by default. During recording, it confines the pointer to RLA's client area while RLA is the foreground window. Stop recording, switch apps, or turn the option off to release it. This keeps the pointer over the same application while comparing mice. It does not change the raw movement counts. Recordings taken outside RLA can have different observed event rates even when RLA retains focus; cursor confinement controls that variable, but does not prove a USB or driver cause.

The UI message loop processes ordinary client and title-bar cursor movement at most once per frame. Buttons, wheels, keys and window messages continue to be processed. Raw mouse reports stay on the separate capture thread. This avoids an unbounded UI drain under continuous mouse movement. See [Godot's explanation of legacy motion processing at high polling rates](https://godotengine.org/article/fixing-high-polling-rate-mice-on-windows/).

## Build and checks

Open `RLA.slnx` in Visual Studio with the C++ desktop tools. Build **Release / x64**. The project uses vcpkg for ImGui, ImPlot, and nlohmann JSON.

Run `./tests/run.ps1` in PowerShell for the regression checks. The tests cover session import/export, recording boundaries, scaling, timing, and ImGui/ImPlot frame generation. Real mouse timing and visible window interaction still require a hardware check.
