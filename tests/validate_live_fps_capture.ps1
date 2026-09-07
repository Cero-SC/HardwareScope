#requires -Version 7.0
param([Parameter(Mandatory)][string]$Directory, [long]$QpcFrequency = [Diagnostics.Stopwatch]::Frequency)
$ErrorActionPreference = 'Stop'
if ($QpcFrequency -le 0) { throw 'QPC frequency must be positive; use the capture host frequency.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
$raw = @(Import-Csv -LiteralPath (Join-Path $Directory 'presentmon.csv') | Where-Object {
    $interval = [double]::Parse($_.MsBetweenPresents, $culture)
    $interval -gt 0.05 -and $interval -le 60000
})
if (-not $raw.Count) { throw 'No positive raw intervals.' }
if (@($raw | Group-Object ProcessID,SwapChainAddress).Count -ne 1) {
    throw 'This independent validator requires one process/stream; do not combine swap chains.'
}
$indices = [Collections.Generic.Dictionary[uint64,int]]::new()
$intervals = [double[]]::new($raw.Count)
for ($i=0; $i -lt $raw.Count; $i++) {
    $indices.Add([uint64]$raw[$i].TimeInQPC, $i)
    $intervals[$i] = [double]::Parse($raw[$i].MsBetweenPresents, $culture)
}
$readings = @(Get-Content -LiteralPath (Join-Path $Directory 'readings.csv') | Select-Object -Skip 1 |
    Where-Object { $_ -match '^elapsed_ms|^[0-9]+,' } | ConvertFrom-Csv)
$lowCache = [Collections.Generic.Dictionary[string,int]]::new()
$available = 0; $dropouts = 0; $fpsMismatches = 0; $lowMismatches = 0
$ages = [Collections.Generic.List[double]]::new()
$updates = [Collections.Generic.List[double]]::new()
$lastQpc = [uint64]0; $lastUpdate = 0.0
foreach ($row in $readings) {
    if ($row.available -ne '1') { if ($available) { $dropouts++ }; continue }
    $available++
    $end = 0
    if (-not $indices.TryGetValue([uint64]$row.frame_qpc, [ref]$end)) { throw 'Snapshot frame is absent from raw data.' }
    $sum = 0.0; $count = 0
    for ($i=$end; $i -ge 0 -and $sum -lt 500; $i--) { $sum += $intervals[$i]; $count++ }
    $expectedFps = [Math]::Clamp([int][Math]::Round(1000*$count/$sum, [MidpointRounding]::AwayFromZero),1,9999)
    if ($expectedFps -ne [int]$row.fps) { $fpsMismatches++ }
    if ([int]$row.one_percent_low -gt 0) {
        $key = "$($row.low_frame_qpc)/$($row.low_interval_count)"
        if (-not $lowCache.ContainsKey($key)) {
            $lowEnd = 0
            if (-not $indices.TryGetValue([uint64]$row.low_frame_qpc, [ref]$lowEnd)) { throw 'Low calculation boundary is absent from raw data.' }
            $historyCount = [int]$row.low_interval_count
            $start = $lowEnd-$historyCount+1
            if ($historyCount -lt 100 -or $start -lt 0) { throw 'Invalid low history boundary.' }
            $sorted = [double[]]::new($historyCount)
            [Array]::Copy($intervals,$start,$sorted,0,$historyCount)
            [Array]::Sort($sorted)
            $slowCount = [int][Math]::Ceiling($historyCount/100.0)
            $slowSum = 0.0
            for($i=$historyCount-$slowCount;$i -lt $historyCount;$i++) { $slowSum += $sorted[$i] }
            $lowCache[$key] = [Math]::Clamp([int][Math]::Round(1000*$slowCount/$slowSum,[MidpointRounding]::AwayFromZero),1,9999)
        }
        if ($lowCache[$key] -ne [int]$row.one_percent_low) { $lowMismatches++ }
    }
    $age = ([double]$row.receipt_qpc-[double]$row.frame_qpc)*1000/$QpcFrequency
    if ($age -lt 0) { throw 'Future frame timestamp in readings.' }
    $ages.Add($age)
    if ([uint64]$row.frame_qpc -ne $lastQpc) {
        if ($lastQpc) { $updates.Add([double]$row.elapsed_ms-$lastUpdate) }
        $lastQpc = [uint64]$row.frame_qpc; $lastUpdate = [double]$row.elapsed_ms
    }
}
$result = [pscustomobject]@{
    RawIntervals=$raw.Count; AvailableSamples=$available; PostStartupUnavailableSamples=$dropouts
    FpsMismatches=$fpsMismatches; LowMismatches=$lowMismatches; DistinctLowCalculations=$lowCache.Count
    AverageFrameAgeMs=($ages | Measure-Object -Average).Average; MaxFrameAgeMs=($ages | Measure-Object -Maximum).Maximum
    AverageUpdateMs=($updates | Measure-Object -Average).Average; MaxUpdateMs=($updates | Measure-Object -Maximum).Maximum
}
$result | Format-List
if ($available -lt 100 -or $lowCache.Count -lt 10 -or $dropouts -or $fpsMismatches -or $lowMismatches) {
    throw 'Live continuity/statistics comparison failed; inspect raw data and history boundaries.'
}
if ($result.MaxFrameAgeMs -gt 1000 -or $result.AverageUpdateMs -gt 250) { throw 'Live delivery remains too slow.' }
Write-Output 'PASS independent raw-frame FPS/1% low and live delivery comparison (single continuous stream).'
