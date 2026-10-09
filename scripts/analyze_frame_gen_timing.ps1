# SPDX-FileCopyrightText: Copyright 2026 IFreemz
# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory)][string]$Path,
    [ValidateRange(1, 86400)][double]$Seconds = 30,
    [ValidateRange(1, 1000)][double]$BaseRate = 60
)

$ErrorActionPreference = 'Stop'
$rows = @(Import-Csv -LiteralPath $Path | Where-Object {
    $_.time_us -match '^\d+$' -and $_.duration_us -match '^\d+$' -and
    $_.frame -match '^\d+$' -and $_.value -match '^\d+$'
})
if ($rows.Count -eq 0) { throw 'No complete timing records found.' }
$end = [long]$rows[-1].time_us
$rows = @($rows | Where-Object { [long]$_.time_us -ge $end - $Seconds * 1e6 })
$frames = @($rows | Where-Object event -eq 'flip_present')
if ($frames.Count -lt 2) { throw 'At least two completed game presents are required.' }

function Get-Intervals($Records) {
    for ($i = 1; $i -lt $Records.Count; ++$i) {
        ([long]$Records[$i].time_us - [long]$Records[$i - 1].time_us) / 1000
    }
}
function Get-Percentile($Values, [double]$Fraction) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    $sorted[[math]::Min($sorted.Count - 1, [math]::Floor($sorted.Count * $Fraction))]
}

$intervals = @(Get-Intervals $frames)
$ticks = @($rows | Where-Object event -eq 'vblank')
$tickIntervals = @(Get-Intervals $ticks)
$numbered = @($rows | Where-Object { $_.event -eq 'present_frame' -and [long]$_.frame -gt 0 })
$regressions = 0
for ($i = 1; $i -lt $numbered.Count; ++$i) {
    if ([long]$numbered[$i].frame -le [long]$numbered[$i - 1].frame) { ++$regressions }
}
$events = @($rows | Group-Object event | ForEach-Object {
    $durations = @($_.Group | ForEach-Object { [long]$_.duration_us / 1000 })
    [pscustomobject]@{
        Event = $_.Name
        Count = $_.Count
        MeanMs = [math]::Round(($durations | Measure-Object -Average).Average, 3)
        P95Ms = Get-Percentile $durations 0.95
        MaxMs = ($durations | Measure-Object -Maximum).Maximum
    }
})
[pscustomobject]@{
    Path = (Resolve-Path -LiteralPath $Path).Path
    Frames = $frames.Count
    BaseFps = [math]::Round(($frames.Count - 1) * 1e6 /
        ([long]$frames[-1].time_us - [long]$frames[0].time_us), 3)
    PresentIntervalP50Ms = Get-Percentile $intervals 0.50
    PresentIntervalP95Ms = Get-Percentile $intervals 0.95
    PresentIntervalP99Ms = Get-Percentile $intervals 0.99
    PresentIntervalMaxMs = ($intervals | Measure-Object -Maximum).Maximum
    LongIntervals = @($intervals | Where-Object { $_ -gt 1500 / $BaseRate }).Count
    VblankIntervalP99Ms = Get-Percentile $tickIntervals 0.99
    FrameIdRegressions = $regressions
    Events = $events
} | ConvertTo-Json -Depth 4
