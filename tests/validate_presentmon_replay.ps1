param([Parameter(Mandatory)][string]$TracePath, [Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
# Offline only: never start/stop a live ETW session or use the global runner probe.
$trace = (Resolve-Path -LiteralPath $TracePath).Path
if ((Get-FileHash -LiteralPath $trace -Algorithm SHA256).Hash -ne '71194538CF20CCE3ADA1FEBCE98DD12E978920DB00ADEE7FDD1AF2886983FAF3') {
    throw 'Expected the pinned upstream v2.4.1 Tests/Gold/test_case_2.etl trace.'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$runtime = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../third_party/presentmon/PresentMon-2.4.1-x64.exe')).Path
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $runtime
$start.Arguments = '--etl_file "' + $trace + '" --output_stdout --no_console_stats --qpc_time --no_track_display --no_track_gpu --no_track_input'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = New-Object Diagnostics.Process
$process.StartInfo = $start
try {
    if (-not $process.Start()) { throw 'Cannot start offline replay.' }
    $outputTask = $process.StandardOutput.ReadToEndAsync()
    $errorTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) { $process.Kill(); $process.WaitForExit(); throw 'Offline replay exceeded 30 seconds.' }
    $csv = Join-Path $directory 'presentmon-replay.csv'
    [IO.File]::WriteAllText($csv, $outputTask.GetAwaiter().GetResult())
    [IO.File]::WriteAllText((Join-Path $directory 'presentmon-replay.stderr.log'), $errorTask.GetAwaiter().GetResult())
    if ($process.ExitCode -ne 0) { throw "Offline replay failed: $($process.ExitCode)" }
    $rows = @(Import-Csv -LiteralPath $csv)
    if ($rows.Count -eq 0) { throw 'Replay produced no frames.' }
    foreach ($field in @('ProcessID','SwapChainAddress','MsBetweenPresents','TimeInQPC')) {
        if ($rows[0].PSObject.Properties.Name -cnotcontains $field) { throw "Missing production field: $field" }
    }
    $positive = @($rows | Where-Object {
        [double]::Parse($_.MsBetweenPresents, [Globalization.CultureInfo]::InvariantCulture) -gt 0.05 -and [uint64]$_.TimeInQPC -gt 0
    })
    if ($positive.Count -lt 20) { throw 'Insufficient positive presentation intervals.' }
    Write-Output "PASS offline bundled PresentMon contract: $($rows.Count) rows, $($positive.Count) positive intervals; required fields present."
    Write-Output 'This verifies offline output compatibility, not live game accuracy or ETW permissions.'
} finally { $process.Dispose() }
