param([switch]$Elevated, [switch]$ReportData)
$ErrorActionPreference = 'Stop'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    if ($Elevated) { throw 'USB tracing requires administrator rights.' }
    $elevationArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'), '-Elevated')
    if ($ReportData) { $elevationArguments += '-ReportData' }
    Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList $elevationArguments
    exit
}

$repoPath = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$probePath = Join-Path $repoPath 'x64/RawInputProbe/RawInputProbe.exe'
if ($ReportData) { $probePath = Join-Path $repoPath 'x64/RawInputReportProbe2/RawInputProbe.exe' }
$resultsPath = Join-Path $env:LOCALAPPDATA 'RLA/RawInputProbe'
$tracePath = Join-Path $resultsPath ('USB-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID)
New-Item -ItemType Directory -Force -Path $tracePath | Out-Null
$sessionName = 'RLA-USB-Probe-' + $PID
$created = $false
$running = $false
$logPath = Join-Path $tracePath 'capture.log'
function Invoke-Logman([string[]]$Arguments) {
    $output = & "$env:SystemRoot/System32/logman.exe" @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $output | Out-File -LiteralPath $logPath -Append
    if ($exitCode -ne 0) { throw "logman failed ($exitCode): $($Arguments -join ' ')" }
}
try {
    if (-not (Test-Path -LiteralPath $probePath)) { throw 'Build RawInputProbe first.' }
    # PartialDataBusTrace is opt-in for the movement-byte comparison.
    Invoke-Logman @('create','trace','-n',$sessionName,'-o',(Join-Path $tracePath 'usb.etl'),'-f','bincirc','-max','256','-nb','64','256','-bs','128')
    $created = $true
    foreach ($provider in @('Microsoft-Windows-USB-USBXHCI','Microsoft-Windows-USB-UCX','Microsoft-Windows-USB-USBHUB3')) {
        $keywords = if ($ReportData) { '0x81' } else { '0x41' }
        Invoke-Logman @('update','trace','-n',$sessionName,'-p',$provider,$keywords,'5')
    }
    $oldFiles = @(Get-ChildItem -LiteralPath $resultsPath -Filter 'probe-*.json' | Select-Object -ExpandProperty FullName)
    Invoke-Logman @('start','-n',$sessionName)
    $running = $true
    $startedUtc = [DateTime]::UtcNow
    # This is the interactive test window, not a background service.
    if ($ReportData) { $probe = Start-Process -FilePath $probePath -ArgumentList '--report-data' -PassThru }
    else { $probe = Start-Process -FilePath $probePath -PassThru }
    if ($ReportData) {
        $readyPath = Join-Path $resultsPath "ready-$($probe.Id).json"
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        while (-not (Test-Path -LiteralPath $readyPath) -and [DateTime]::UtcNow -lt $deadline -and -not $probe.HasExited) { Start-Sleep -Milliseconds 100 }
        if (-not (Test-Path -LiteralPath $readyPath)) { throw 'The probe did not confirm report mode. Do not record.' }
        $ready = Get-Content -LiteralPath $readyPath -Raw | ConvertFrom-Json
        if (-not $ready.reportData -or $ready.version -ne 2) { throw 'Incorrect probe mode. Do not record.' }
        Copy-Item -LiteralPath $readyPath -Destination $tracePath
    }
    $result = $null
    while (([DateTime]::UtcNow - $startedUtc).TotalSeconds -lt 120) {
        $result = Get-ChildItem -LiteralPath $resultsPath -Filter "probe-*-$($probe.Id).json" |
            Where-Object { $_.FullName -notin $oldFiles } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if ($result -or $probe.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }
    Invoke-Logman @('stop','-n',$sessionName)
    $running = $false
    if ($result) { Copy-Item -LiteralPath $result.FullName -Destination $tracePath }
    if ($ReportData -and $result) {
        $recorded = Get-Content -LiteralPath $result.FullName -Raw | ConvertFrom-Json
        if ($recorded.version -ne 2 -or -not $recorded.clockAnchor -or -not $recorded.samples) { throw 'Report capture is incomplete: X/Y samples or clock anchor are missing.' }
    }
    [ordered]@{
        traceStartedUtc=$startedUtc.ToString('o'); traceStoppedUtc=[DateTime]::UtcNow.ToString('o')
        probePid=$probe.Id; resultFile=if($result){$result.Name}else{$null}
        resultWrittenUtc=if($result){$result.LastWriteTimeUtc.ToString('o')}else{$null}
        keywords=if($ReportData){'Default, PartialDataBusTrace'}else{'Default, HeadersBusTrace'}; maximumMiB=256
        note=if($ReportData){'Contains USB payload bytes. Check ETW loss and sampleOverflow. Use the probe clockAnchor for alignment; timestamps measure driver/application processing, not electrical arrival.'}else{'USB driver events, not electrical bus measurements. Check ETW event loss before comparing counts. Stage alignment from result file time is approximate.'}
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $tracePath 'capture.json') -Encoding utf8
} catch {
    $_ | Out-File -LiteralPath $logPath -Append
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show("USB trace failed. See $logPath`n$_", 'USB trace') | Out-Null
} finally {
    if ($running) { & "$env:SystemRoot/System32/logman.exe" stop -n $sessionName 2>&1 | Out-File -LiteralPath $logPath -Append }
    if ($created) { & "$env:SystemRoot/System32/logman.exe" delete -n $sessionName 2>&1 | Out-File -LiteralPath $logPath -Append }
}
