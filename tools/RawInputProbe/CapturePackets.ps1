param([switch]$Elevated, [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    if ($Elevated) { throw 'Administrator rights are required.' }
    $launchArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),'-Elevated')
    if ($CheckOnly) { $launchArgs += '-CheckOnly' }
    Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList $launchArgs
    exit
}
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$exe = Join-Path $root 'x64/RawInputReportProbe2/RawInputProbe.exe'
$usb = 'C:/Program Files/USBPcap/USBPcapCMD.exe'
$base = Join-Path $env:LOCALAPPDATA 'RLA/RawInputProbe'
$out = Join-Path $base ('Packets-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID)
New-Item -ItemType Directory -Path $out -Force | Out-Null
$captures = @()
$metadata = [ordered]@{ startedUtc=[DateTime]::UtcNow.ToString('o'); complete=$false; error=$null; captures=@(); result=$null }
try {
    $map = Get-Content -LiteralPath (Join-Path $base 'packet-devices.json') -Raw | ConvertFrom-Json
    if (@($map).Count -ne 2) { throw 'Two verified mouse capture targets are required.' }
    foreach ($target in $map) {
        $info = New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName = $usb
        $info.Arguments = '--extcap-interface "' + $target.interface + '" --extcap-config'
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $enumerator = [Diagnostics.Process]::Start($info)
        $config = ($enumerator.StandardOutput.ReadToEnd() -replace "`r`n","`n").TrimEnd("`r","`n")
        $enumerator.WaitForExit()
        $config | Set-Content -LiteralPath (Join-Path $out (($target.interface -replace '^.*USBPcap','hub') + '-devices.txt')) -Encoding utf8
        if ($enumerator.ExitCode -ne 0 -or $config -cne $target.config) { throw 'USB topology changed. Recheck mouse capture targets before recording.' }
    }
    $check = Start-Process $exe -ArgumentList '--report-data --verify-report-data' -WindowStyle Hidden -PassThru -Wait
    if ($check.ExitCode -ne 0) { throw 'Probe report-mode check failed.' }
    foreach ($target in $map) {
        $name = $target.interface -replace '^.*USBPcap','hub'
        $pcap = Join-Path $out ($name + '.pcap')
        $capture = Start-Process $usb -ArgumentList @('--capture','--extcap-interface',$target.interface,'--devices',$target.address,'--inject-descriptors','--snaplen','65535','--bufferlen','4194304','--fifo',('"'+$pcap+'"')) -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $out ($name+'.stderr.txt'))
        $captures += $capture
        $metadata.captures += @{ interface=$target.interface; address=$target.address; file=[IO.Path]::GetFileName($pcap); pid=$capture.Id }
    }
    Start-Sleep -Milliseconds 500
    foreach ($capture in $captures) { if ($capture.HasExited) { throw 'USB packet capture failed to start. See stderr files.' } }
    if ($CheckOnly) {
        Start-Sleep -Seconds 2
        $metadata.checkOnly = $true
        $metadata.complete = $true
    } else {
    $probe = Start-Process $exe -ArgumentList '--report-data' -PassThru
    $readyPath = Join-Path $base "ready-$($probe.Id).json"
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $readyPath) -and [DateTime]::UtcNow -lt $deadline -and -not $probe.HasExited) { Start-Sleep -Milliseconds 100 }
    $ready = Get-Content -LiteralPath $readyPath -Raw | ConvertFrom-Json
    if (-not $ready.reportData -or $ready.version -ne 2) { throw 'Probe did not confirm report mode.' }
    Copy-Item -LiteralPath $readyPath -Destination $out
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    $result = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        foreach ($capture in $captures) { if ($capture.HasExited) { throw 'USB packet capture exited early.' } }
        $bytes = (Get-ChildItem -LiteralPath $out -Filter '*.pcap' | Measure-Object -Property Length -Sum).Sum
        if ($bytes -gt 268435456) { throw 'Capture reached the 256 MiB size limit.' }
        $result = Get-ChildItem -LiteralPath $base -Filter "probe-*-$($probe.Id).json" | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if ($result -or $probe.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $result) { throw 'No probe result was saved before the deadline.' }
    Copy-Item -LiteralPath $result.FullName -Destination $out
    $j = Get-Content -LiteralPath $result.FullName -Raw | ConvertFrom-Json
    if ($j.version -ne 2 -or -not $j.clockAnchor -or -not $j.samples -or $j.sampleOverflow -ne 0 -or -not $j.complete) { throw 'Probe result is incomplete or report storage overflowed.' }
    $metadata.result = $result.Name
    # Keep a margin after the measured windows before stopping the capture processes.
    Start-Sleep -Seconds 2
    $metadata.complete = $true
    }
} catch {
    $metadata.error = $_.ToString()
} finally {
    # Stop only the processes started by this helper. PCAP readers must ignore a partial final record.
    foreach ($capture in $captures) { if (-not $capture.HasExited) { Stop-Process -Id $capture.Id -ErrorAction SilentlyContinue; $capture.WaitForExit(3000) | Out-Null } }
    $metadata.stoppedUtc = [DateTime]::UtcNow.ToString('o')
    $metadata.note = 'Filtered USB device capture. Processes terminated after measurement; check final-record truncation, timestamp coverage and payload lengths before analysis. Driver overflow is not measured by this helper.'
    $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'capture.json') -Encoding utf8
}
if ($metadata.error) {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show("Packet capture failed: $($metadata.error)`nFiles: $out",'USB packet comparison') | Out-Null
}
