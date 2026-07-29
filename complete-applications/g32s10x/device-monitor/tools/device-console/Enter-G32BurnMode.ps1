param(
    [string]$Port = 'COM39',
    [ValidateSet('None', 'DtrResetRtsBoot', 'RtsResetDtrBoot')]
    [string]$Profile = 'None',
    [int]$BaudRate = 115200,
    [ValidateRange(5, 300)]
    [int]$WaitSeconds = 60,
    [switch]$SignalOnly
)

$ErrorActionPreference = 'Stop'

try {
    $melody = @(
        @(659, 180), @(659, 180), @(698, 180), @(784, 280),
        @(784, 180), @(698, 180), @(659, 180), @(587, 360)
    )
    foreach ($note in $melody) {
        [Console]::Beep($note[0], $note[1])
        Start-Sleep -Milliseconds 45
    }
} catch {
    1..3 | ForEach-Object {
        [System.Media.SystemSounds]::Exclamation.Play()
        Start-Sleep -Milliseconds 350
    }
}

Write-Host 'Burn-mode melody played.'
if ($Profile -eq 'None') {
    Write-Host 'Please hold BOOT, press and release RESET, then release BOOT.'
} else {
    $serial = [System.IO.Ports.SerialPort]::new($Port, $BaudRate)
    try {
        $serial.Open()
        $bootUsesRts = $Profile -eq 'DtrResetRtsBoot'
        if ($bootUsesRts) {
            $serial.RtsEnable = $true
            $serial.DtrEnable = $true
            Start-Sleep -Milliseconds 120
            $serial.DtrEnable = $false
            Start-Sleep -Milliseconds 120
            $serial.RtsEnable = $false
        } else {
            $serial.DtrEnable = $true
            $serial.RtsEnable = $true
            Start-Sleep -Milliseconds 120
            $serial.RtsEnable = $false
            Start-Sleep -Milliseconds 120
            $serial.DtrEnable = $false
        }
        Start-Sleep -Milliseconds 600
    } finally {
        if ($serial.IsOpen) { $serial.Close() }
    }
}

if ($SignalOnly) {
    return
}

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$burn = @()
do {
    $burn = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'VID_A108' })
    if ($burn.Count -gt 0) {
        break
    }
    Start-Sleep -Milliseconds 250
} while ((Get-Date) -lt $deadline)

if ($burn.Count -eq 0) {
    throw "VID_A108 did not enumerate within $WaitSeconds seconds; flashing was not started."
}
Write-Host 'Ingenic ROM burn device detected:'
$burn | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize
