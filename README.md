# Relative Latency Analyzer

RLA records movement from two mice through Windows Raw Input. Use the plots to compare their movement and timing.

## Use

1. RLA starts on the recording screen, with A and B at the top. Assign each mouse by moving it. **Mouse settings** beside each device opens its saved identity and connection settings.
2. Record both mice moving together through several smooth cycles. Click or press Esc to stop raw capture.
3. Select **Analyze latency** to compare the curves and fit their displayed heights.
4. Use **Previous**, **Next**, and **Zoom to match** to inspect a match. **Highlight selected match** adds the thick lines and labels; it starts off and navigation does not turn it on. **All matches** controls the other timestamp lines independently.
5. Use **Library / rankings** beside the device controls to manage profiles and results. **Back to recording** returns to the graph.

Select **Event timing / Hz** in **Plot Mode** to inspect intervals and event rates.

In **Movement magnitude**, select **Analyze latency** after recording. Under **Method**, choose **Minima**, **Half-height**, or **Both**. Half-height compares the 50% points between each trough and its neighboring peak, on both the rising and falling slopes. A minimum uses a fit across the bottom of the curve, rather than one noisy sample. Slopes can still be used when the bottom does not support a precise minimum fit.

The estimate uses common time bins and checks local shape, direction, gaps, and agreement between matched features. It requires at least three shared cycles, and searches for differences within +/-10 ms. Independent movement, pauses, weak features, and conflicting estimates are rejected. Each accepted cycle has one vote in the median result. **B-A** is positive when B appears later. **Spread (MAD)** is the median absolute deviation of the accepted point differences; it describes variation, not measurement accuracy. The analysis uses its own bin width. Different DPI and event rates do not require manual amplitude scaling for this fit.

Both curves use the same centered smoothing window, with a 3 ms radius rounded to whole bins. Minima use a squared-magnitude fit over a 10 ms radius of that smoothed curve. This reduces sensitivity to count quantization and arrival jitter without applying a time shift to either recording. The displayed bin width is not an accuracy guarantee. Failed fits report the candidate or accepted-cycle count. The saved events remain unchanged.

**Latency details** lists each accepted feature and both timestamps. Select a row to center the graph there. When **Highlight selected match** is enabled, the selected match has thick A/B lines, timestamp labels, and a B-A bracket. **All matches** adds the other timestamps. Zoom fits both axes around the selected match. Analysis runs in the background and uses its own bins. The Analyze button also runs display amplitude fitting; this does not change the latency calculation or saved events. This is a motion-based estimate with application timestamps, not a measurement of isolated USB or sensor latency. Mechanical differences, sensor processing, capture batching, and quantization can affect the result. No result is shown if the checks fail.

Completed recordings with movement are saved automatically to `%LOCALAPPDATA%\RLA\Autosaves`. Saving runs on a background worker, and each recording gets a new JSON file. A complete temporary file is renamed before it appears as a `.json` autosave. Normal app shutdown finishes pending saves. Autosaves are not a crash-recovery log for an unfinished recording. Use **Autosaves > Open folder** to find the files, then **File > Load Session** to open one. **Clear autosaves** asks before deleting only `RLA-autosave-*.json` files in that folder; it preserves other files, directories, and the current recording. Save errors appear in the status bar. There is no automatic deletion by age or size.

## Mice and ranking

Use **Mouse settings** beside A or B to name and link a device. Open **Library / rankings > Mouse library** to create or rename profiles. Add a wired or wireless setup under the same profile, then link each connected mouse to its setup. RLA remembers the Windows device interface. A new receiver or USB port can require a manual link. **Link setup to mouse** moves an existing setup under another profile without combining its results. Windows supplies an interface name through [GetRawInputDeviceInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getrawinputdeviceinfow); that name alone does not identify wired and wireless connections as the same physical mouse.

After recording, RLA estimates Hz from steady movement and separates the detected polling rates. This is an arrival-rate estimate, not a device setting read from the mouse. Sparse movement, short recordings, and heavily grouped arrivals leave Hz unspecified and are not ranked. Move faster for a new recording, or assign a known-rate setup under **Rankings > Check recording labels and save result**. DPI does not create separate entries. Use **Setup notes** for performance mode, firmware, receiver placement, or other settings that you want to compare separately.

**Analyze and add valid results to rankings automatically** is on by default. It requires two linked setups with known Hz and a valid curve analysis. The ranking uses the median result for each pair, then fits the relative differences through shared references, weighted by the number of runs. Only setups in the same connected group can be ranked together. Minima, half-height, and combined results stay separate. Lower values mean earlier movement within the group; zero is its fitted earliest setup, not zero device latency. **Group mismatch** reports disagreement between comparison paths, not measurement accuracy.

Every accepted recording is kept in `%LOCALAPPDATA%\RLA\Runs`. Profiles, interface links, and results are saved in `%LOCALAPPDATA%\RLA\mice.json`. **Saved runs** can open recordings and exclude individual results from the ranking. **Open best** marks the smallest spread against the same reference and method; ties favor more accepted cycles. It does not choose the lowest latency value. Reanalyzing one recording replaces its result for that method instead of adding weight. Clearing autosaves preserves this library and its retained runs. Saving runs in the background, reports errors, and finishes during normal shutdown. A second RLA instance can view the library but cannot change it.

## Plot and capture details

Movement plots start in a smoothed comparison view: common 2 ms time-weighted bins and a centered three-bin average. **Smooth comparison** restores these settings. **Raw reports** disables binning, smoothing, and gap filling. Individual controls are under **Advanced display settings**. These settings affect the display only; saved counts and automatic latency analysis are unchanged.

Prepared movement curves are cached until their recording, event count, or processing settings change. Panning and zooming a stopped recording reuse the cached curves. During recording, new events refresh the movement display at most about 30 times per second. Display setting changes bypass this limit, and stopping immediately includes all final events. Capture still retains every delivered movement event; the display limit does not alter the input thread. This reduces live plot work but does not establish the source of capture jitter. The UI uses a high-resolution waitable timer with a 120 Hz update limit, instead of the coarse 16 ms tick-based limiter. Actual display rate still depends on rendering and the monitor. The capture thread does not use this timer. See [Microsoft: high-resolution waitable timers](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw).

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

Capture runs on a dedicated thread. `GetMessageW` waits for an input message. Its `WM_INPUT` handler reads that report with `GetRawInputData`, then drains the remaining queued reports with `GetRawInputBuffer`. Each report keeps its own device and movement counts. The reusable buffer grows if required. This follows [Microsoft's combined standard and buffered input guidance](https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input). The active capture loop has no sleep or timer delay. The previous empty-wakeup sleep has been removed; it could turn nominally short waits into coarse batches. [Microsoft documents that Sleep timing depends on the system clock resolution.](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep)

Windows does not supply a device timestamp in `RAWINPUT`. All reports returned in one read therefore share the QPC time taken after that read. Zero intervals within a batch are expected; they are not measured zero-duration USB intervals. The window rate still counts each movement event. Buffered capture cannot recover original report times or split reports already combined upstream. Existing recordings remain unchanged.

**Summary** shows capture totals since application start: mouse reports, reports read in groups, largest batch, application queue drops, and API/packet errors. These totals include all received mice and activity outside the recording. They are saved as optional `capture` metadata and frozen when recording stops. Older JSON sessions remain supported. Use a new recording to check high-rate capture; synthetic regression checks cannot confirm physical USB performance.

**Raw capture** is on by default for new recordings. It adds `RIDEV_NOLEGACY` to the mouse registration only while recording, so Windows does not generate ordinary mouse messages for RLA during capture. This removes that input path instead of only limiting how often the UI processes it. Click either mouse's left button or press **Esc** to stop. Other mouse controls are paused until recording stops. Switching away from RLA also stops this mode and restores ordinary mouse controls. Turn **Raw capture** off before recording for comparison with the previous input path. The mode is shown in **Summary** and saved as `capture.legacySuppressed`. Older recordings without this field load as off. See [Microsoft: RAWINPUTDEVICE](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawinputdevice).

This is a capture-path change to test with physical mice. It does not reconstruct reports already combined before the application reads them or guarantee a particular observed event rate.

**Keep cursor in window** is on by default. During recording, it confines the pointer to RLA's client area while RLA is the foreground window. Stop recording, switch apps, or turn the option off to release it. This keeps the pointer over the same application while comparing mice. It does not change the raw movement counts. Recordings taken outside RLA can have different observed event rates even when RLA retains focus; cursor confinement controls that variable, but does not prove a USB or driver cause.

The UI message loop processes ordinary client and title-bar cursor movement at most once per frame. Buttons, wheels, keys and window messages continue to be processed. Raw mouse reports stay on the separate capture thread. This avoids an unbounded UI drain under continuous mouse movement. See [Godot's explanation of legacy motion processing at high polling rates](https://godotengine.org/article/fixing-high-polling-rate-mice-on-windows/).

Each UI frame also limits other queued messages and consumes a fixed snapshot of input events. The capture thread limits each buffered drain and then returns to its blocking message loop. It uses high priority instead of time-critical priority. Long idle gaps add a bounded number of plot points. These changes prevent specific loops from blocking UI progress; they do not prove the cause of an intermittent freeze. See [Microsoft: Scheduling priorities](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities).

Graphics-buffer resize requests are deferred until the next frame and coalesced. Render targets are unbound before resizing. Presentation uses `DXGI_PRESENT_DO_NOT_WAIT`, while the main loop limits frame frequency and continues to process window messages. This removes the wait for presentation-queue space from the window thread. It does not guarantee that all driver calls will return promptly. See [Microsoft: DXGI presentation flags](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present) and [DXGI window-message interaction](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/dxgi-best-practices).

The monitor writes a startup entry to `%LOCALAPPDATA%\RLA\Diagnostics\hang-<process-id>.txt`. If the main loop stops progressing for five seconds, it writes the last loop phase before checking whether the window answers messages. A driver or modal loop can answer messages while frame progress is stopped; the previous monitor incorrectly suppressed these reports. Long file-dialog waits can also produce entries, so an entry alone does not prove a hang. This local log contains no memory dump or mouse data. Native replay tests do not reproduce every hardware or driver condition.

## Build and checks

Open `RLA.slnx` in Visual Studio with the C++ desktop tools. Build **Release / x64**. The project uses vcpkg for ImGui, ImPlot, and nlohmann JSON.

Run `./tests/run.ps1` in PowerShell for the regression checks. The tests cover session import/export, recording boundaries, scaling, timing, and ImGui/ImPlot frame generation. Real mouse timing and visible window interaction still require a hardware check.
