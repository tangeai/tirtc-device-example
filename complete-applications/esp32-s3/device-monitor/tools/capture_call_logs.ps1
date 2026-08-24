param(
    [string]$Port,
    [string]$BuildDir = "build-contact-cloud"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$settingsPath = Join-Path $projectRoot ".vscode\settings.json"

if ([string]::IsNullOrWhiteSpace($Port) -and (Test-Path -LiteralPath $settingsPath)) {
    $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
    $Port = $settings.'idf.portWin'
}
if ([string]::IsNullOrWhiteSpace($Port)) {
    throw "Serial port is required. Example: .\tools\capture_call_logs.ps1 -Port COM37"
}

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    $idfExport = "C:\esp\v5.5.4\esp-idf\export.ps1"
    if (-not (Test-Path -LiteralPath $idfExport)) {
        throw "idf.py is unavailable and ESP-IDF export script was not found: $idfExport"
    }
    . $idfExport
}

$resolvedBuildDir = Join-Path $projectRoot $BuildDir
if (-not (Test-Path -LiteralPath $resolvedBuildDir)) {
    throw "Build directory does not exist: $resolvedBuildDir"
}

$logDir = Join-Path $projectRoot "debug_logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $logDir "call-$Port-$timestamp.log"

Write-Host "Capturing the complete device log from $Port"
Write-Host "Reproduce one full call, then press Ctrl+] to stop the monitor."
Write-Host "Log file: $logPath"

Push-Location $projectRoot
try {
    & idf.py -B $BuildDir -p $Port monitor 2>&1 | Tee-Object -FilePath $logPath
} finally {
    Pop-Location
    Write-Host "Saved: $logPath"
}
