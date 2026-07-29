param(
    [string]$Url = 'http://127.0.0.1:8787/',
    [string]$BuildingDir,
    [int]$TimeoutSeconds = 8
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($BuildingDir)) {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $BuildingDir = Join-Path $repoRoot 'building'
}
$base = $Url.TrimEnd('/')
$health = Invoke-RestMethod -Uri "$base/api/health" -TimeoutSec $TimeoutSeconds
$status = Invoke-RestMethod -Uri "$base/api/device/status" -TimeoutSec $TimeoutSeconds
$logs = Invoke-RestMethod -Uri "$base/api/logs?tail=120" -TimeoutSec $TimeoutSeconds
$html = Invoke-WebRequest -UseBasicParsing -Uri "$base/" -TimeoutSec $TimeoutSeconds

if (-not $health.ok -or $health.service -ne 'g32-device-console') {
    throw 'Device-console health contract failed.'
}
if (-not $status.ok -or $status.chip -ne 'G32S10X' -or
    $status.width -ne 480 -or $status.height -ne 854) {
    throw 'Device status contract failed.'
}
if (-not $logs.ok -or -not $logs.file -or $logs.lines.Count -eq 0) {
    throw 'Serial log endpoint returned no G32 log data.'
}
if ($html.Content -notmatch 'id="device-screen"' -or
    $html.Content -notmatch 'id="log-output"') {
    throw 'Dashboard HTML is missing the screen or log surface.'
}

New-Item -ItemType Directory -Force -Path $BuildingDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bmp = Join-Path $BuildingDir "g32_console_screen_$stamp.bmp"
Invoke-WebRequest -UseBasicParsing -Uri "$base/api/device/screen.bmp" `
    -TimeoutSec $TimeoutSeconds -OutFile $bmp
$bytes = [System.IO.File]::ReadAllBytes($bmp)
if ($bytes.Length -lt 54 -or $bytes[0] -ne 0x42 -or $bytes[1] -ne 0x4D) {
    throw 'Dashboard screen proxy did not return a BMP.'
}
$width = [BitConverter]::ToInt32($bytes, 18)
$height = [Math]::Abs([BitConverter]::ToInt32($bytes, 22))
if ($width -ne 480 -or $height -ne 854) {
    throw "Dashboard BMP dimensions are invalid: ${width}x${height}"
}

[pscustomobject]@{
    Url = $Url
    DeviceIp = $status.ip
    Chip = $status.chip
    Screen = "${width}x${height}"
    LogFile = $logs.file
    LogLines = $logs.lines.Count
    Bmp = $bmp
    Sha256 = (Get-FileHash -LiteralPath $bmp -Algorithm SHA256).Hash
}
