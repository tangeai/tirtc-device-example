param(
    [string]$DeviceIp,
    [ValidateRange(1, 65535)]
    [int]$DevicePort = 8080,
    [string]$ListenAddress = '127.0.0.1',
    [ValidateRange(1, 65535)]
    [int]$Port = 8787,
    [string]$BuildingDir,
    [string]$ComPort = 'COM39',
    [ValidateRange(60, 604800)]
    [int]$SerialSeconds = 86400,
    [switch]$NoSerialMonitor,
    [switch]$Foreground,
    [switch]$Restart
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildingDir) {
    $BuildingDir = Join-Path $repo 'building'
}
$building = [System.IO.Path]::GetFullPath($BuildingDir)
$serverScript = Join-Path $PSScriptRoot 'server\g32_device_console.py'
$webRoot = Join-Path $PSScriptRoot 'web'
$runtimePath = Join-Path $building 'device_console_runtime.json'
$url = 'http://{0}:{1}/' -f $ListenAddress, $Port

New-Item -ItemType Directory -Force -Path $building | Out-Null
foreach ($required in @($serverScript, (Join-Path $webRoot 'index.html'))) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing device-console file: $required"
    }
}

function Get-ConsoleProcess {
    Get-CimInstance Win32_Process |
        Where-Object {
            $_.Name -match '^python(?:3)?(?:\.exe)?$' -and
            $_.CommandLine -like '*g32_device_console.py*'
        } |
        Select-Object -First 1
}

function Get-SerialMonitorProcess {
    Get-CimInstance Win32_Process |
        Where-Object {
            $_.ProcessId -ne $PID -and
            $_.Name -eq 'powershell.exe' -and
            $_.CommandLine -match '-File\s+[^\r\n]*Read-G32SerialLog\.ps1'
        } |
        Select-Object -First 1
}

$existingConsole = Get-ConsoleProcess
if ($existingConsole -and $Restart) {
    Stop-Process -Id $existingConsole.ProcessId -Force
    Start-Sleep -Milliseconds 500
    $existingConsole = $null
}
if ($existingConsole) {
    try {
        $health = Invoke-RestMethod -Uri ($url + 'api/health') -TimeoutSec 2
        [pscustomobject]@{
            Url = $url
            ServerPid = $existingConsole.ProcessId
            SerialPid = (Get-SerialMonitorProcess).ProcessId
            DeviceIp = $health.device_ip
            Reused = $true
        }
        return
    } catch {
        throw "A device-console process is already running but $url is not healthy. Use -Restart."
    }
}

$serialProcess = Get-SerialMonitorProcess
if (-not $NoSerialMonitor -and -not $serialProcess) {
    $serialScript = Join-Path $repo 'scripts\Read-G32SerialLog.ps1'
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $serialOut = Join-Path $building "console_serial_${stamp}.stdout.log"
    $serialErr = Join-Path $building "console_serial_${stamp}.stderr.log"
    $serialArgs = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $serialScript,
        '-Port', $ComPort, '-Seconds', "$SerialSeconds", '-LogDir', $building
    )
    $serialProcess = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') `
        -ArgumentList $serialArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $serialOut -RedirectStandardError $serialErr
}

$python = (Get-Command python -ErrorAction Stop).Source
$serverArgs = @(
    $serverScript,
    '--host', $ListenAddress,
    '--port', "$Port",
    '--device-port', "$DevicePort",
    '--com-port', $ComPort,
    '--building-dir', $building,
    '--web-root', $webRoot
)
if ($DeviceIp) {
    $serverArgs += @('--device-ip', $DeviceIp)
}

if ($Foreground) {
    & $python @serverArgs
    exit $LASTEXITCODE
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$serverOut = Join-Path $building "device_console_${stamp}.stdout.log"
$serverErr = Join-Path $building "device_console_${stamp}.stderr.log"
$serverProcess = Start-Process -FilePath $python -ArgumentList $serverArgs `
    -WindowStyle Hidden -PassThru -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr

$health = $null
for ($attempt = 0; $attempt -lt 40; $attempt++) {
    Start-Sleep -Milliseconds 250
    if ($serverProcess.HasExited) {
        $errorText = if (Test-Path -LiteralPath $serverErr) {
            Get-Content -LiteralPath $serverErr -Raw -ErrorAction SilentlyContinue
        } else { '' }
        throw "Device console exited during startup. $errorText"
    }
    try {
        $health = Invoke-RestMethod -Uri ($url + 'api/health') -TimeoutSec 1
        if ($health.ok) { break }
    } catch {
    }
}
if (-not $health -or -not $health.ok) {
    Stop-Process -Id $serverProcess.Id -Force -ErrorAction SilentlyContinue
    throw "Device console did not become healthy at $url"
}

$serialPid = if ($serialProcess) {
    if ($serialProcess.PSObject.Properties['ProcessId']) {
        $serialProcess.ProcessId
    } else {
        $serialProcess.Id
    }
} else {
    $null
}

$runtime = [ordered]@{
    url = $url
    server_pid = $serverProcess.Id
    serial_pid = $serialPid
    device_ip = $health.device_ip
    device_port = $DevicePort
    com_port = $ComPort
    started_at = (Get-Date).ToString('o')
    stdout = $serverOut
    stderr = $serverErr
}
[System.IO.File]::WriteAllText(
    $runtimePath,
    ($runtime | ConvertTo-Json -Depth 3),
    [System.Text.UTF8Encoding]::new($false)
)

[pscustomobject]@{
    Url = $url
    ServerPid = $serverProcess.Id
    SerialPid = $runtime.serial_pid
    DeviceIp = $health.device_ip
    Reused = $false
    Runtime = $runtimePath
}
