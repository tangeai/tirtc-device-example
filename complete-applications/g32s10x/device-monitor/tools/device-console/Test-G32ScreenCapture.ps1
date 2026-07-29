param(
    [string]$DeviceIp,
    [string]$BuildingDir,
    [int]$Port = 8080,
    [int]$TimeoutSeconds = 5
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($BuildingDir)) {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $BuildingDir = Join-Path $repoRoot 'building'
}

function Test-ScreenEndpoint {
    param([string]$Ip)
    try {
        $uri = 'http://{0}:{1}/api/status' -f $Ip, $Port
        $response = Invoke-WebRequest -UseBasicParsing -Uri $uri -TimeoutSec 2
        return $response.StatusCode -eq 200 -and $response.Content -match '"chip":"G32S10X"'
    } catch {
        return $false
    }
}

if (-not $DeviceIp) {
    $logs = Get-ChildItem $BuildingDir -File -Filter '*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    foreach ($log in $logs) {
        $match = Select-String -Path $log.FullName -Pattern '\[screen_debug\] ready ip=(\d+\.\d+\.\d+\.\d+)' |
            Select-Object -Last 1
        if ($match) {
            $DeviceIp = $match.Matches[0].Groups[1].Value
            break
        }
    }
}

if (-not $DeviceIp) {
    $wlanIp = Get-NetIPAddress -InterfaceAlias 'WLAN' -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty IPAddress
    if (-not $wlanIp) {
        throw 'Cannot determine the WLAN IPv4 subnet.'
    }
    $prefix = $wlanIp.Substring(0, $wlanIp.LastIndexOf('.') + 1)
    $candidates = @(Get-NetNeighbor -InterfaceAlias 'WLAN' -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -like "$prefix*" -and $_.IPAddress -ne $wlanIp } |
        Select-Object -ExpandProperty IPAddress -Unique)
    foreach ($candidate in $candidates) {
        if (Test-ScreenEndpoint $candidate) {
            $DeviceIp = $candidate
            break
        }
    }
}

if (-not $DeviceIp -or -not (Test-ScreenEndpoint $DeviceIp)) {
    throw 'G32 screen endpoint was not found on the current WLAN.'
}

New-Item -ItemType Directory -Force -Path $BuildingDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bmpPath = Join-Path $BuildingDir "g32_screen_$stamp.bmp"
$statusPath = Join-Path $BuildingDir "g32_screen_status_$stamp.json"
$baseUrl = 'http://{0}:{1}' -f $DeviceIp, $Port
$status = Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/status" -TimeoutSec $TimeoutSeconds
[System.IO.File]::WriteAllText($statusPath, $status.Content, [System.Text.UTF8Encoding]::new($false))
Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/screen.bmp" -TimeoutSec $TimeoutSeconds -OutFile $bmpPath

$bytes = [System.IO.File]::ReadAllBytes($bmpPath)
if ($bytes.Length -lt 54 -or $bytes[0] -ne 0x42 -or $bytes[1] -ne 0x4D) {
    throw "Invalid BMP response: $bmpPath"
}
$width = [BitConverter]::ToInt32($bytes, 18)
$height = [Math]::Abs([BitConverter]::ToInt32($bytes, 22))
$offset = [BitConverter]::ToInt32($bytes, 10)
$colors = [System.Collections.Generic.HashSet[int]]::new()
$nonBlack = 0
$samples = 0
for ($index = $offset; $index + 2 -lt $bytes.Length; $index += 997) {
    $color = $bytes[$index] -bor ($bytes[$index + 1] -shl 8) -bor ($bytes[$index + 2] -shl 16)
    [void]$colors.Add($color)
    if ($color -ne 0) { $nonBlack++ }
    $samples++
}
if ($width -ne 480 -or $height -ne 854 -or $colors.Count -lt 4 -or $nonBlack -eq 0) {
    throw "BMP validation failed width=$width height=$height colors=$($colors.Count) nonblack=$nonBlack/$samples"
}

[pscustomobject]@{
    DeviceIp = $DeviceIp
    Url = "$baseUrl/"
    Status = $status.Content
    Bmp = $bmpPath
    Width = $width
    Height = $height
    SampleColors = $colors.Count
    NonBlackSamples = "$nonBlack/$samples"
    Sha256 = (Get-FileHash -LiteralPath $bmpPath -Algorithm SHA256).Hash
}
