param(
    [ValidateSet('Preflight', 'Build', 'Flash', 'BurnMode', 'VerifyFlash', 'Monitor', 'Dev')]
    [string]$Action = 'Dev',
    [string]$ComPort = 'COM39',
    [ValidateSet('None', 'DtrResetRtsBoot', 'RtsResetDtrBoot')]
    [string]$BootProfile = 'None',
    [int]$LogSeconds = 180,
    [ValidateRange(5, 300)]
    [int]$BurnModeWaitSeconds = 60,
    [switch]$FirmwareOnly,
    [string]$Distro = 'Ubuntu',
    [string]$G32SdkRoot = $env:G32_SDK_ROOT,
    [string]$G32ToolchainBin = $env:G32_TOOLCHAIN_BIN,
    [string]$RepoRoot,
    [string]$BuildingDir,
    [string]$FlashScript = $env:G32_FLASH_SCRIPT,
    [string]$SerialLogScript = $env:G32_SERIAL_LOG_SCRIPT
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$repo = (Resolve-Path -LiteralPath $RepoRoot).Path
if ([string]::IsNullOrWhiteSpace($BuildingDir)) {
    $BuildingDir = Join-Path $repo 'building'
}
$building = [System.IO.Path]::GetFullPath($BuildingDir)
$sessionStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$sessionTempLog = Join-Path $env:TEMP "g32_devloop_$sessionStamp.log"
$preflightTempLog = Join-Path $env:TEMP "g32_preflight_$sessionStamp.log"

function Save-SessionLogs {
    New-Item -ItemType Directory -Force -Path $building | Out-Null
    if (Test-Path -LiteralPath $sessionTempLog) {
        Copy-Item -LiteralPath $sessionTempLog -Destination (Join-Path $building "devloop_$sessionStamp.log") -Force
    }
    if (Test-Path -LiteralPath $preflightTempLog) {
        Copy-Item -LiteralPath $preflightTempLog -Destination (Join-Path $building "preflight_$sessionStamp.log") -Force
    }
}

function Test-PowerShellSources {
    $files = @(
        (Join-Path $PSScriptRoot 'Invoke-G32DevLoop.ps1'),
        (Join-Path $PSScriptRoot 'Enter-G32BurnMode.ps1'),
        (Join-Path $PSScriptRoot 'Wait-G32FlashResult.ps1'),
        (Join-Path $PSScriptRoot 'Test-G32ScreenCapture.ps1'),
        (Join-Path $PSScriptRoot 'Start-G32DeviceConsole.ps1'),
        (Join-Path $PSScriptRoot 'Test-G32DeviceConsole.ps1'),
        (Join-Path $repo 'scripts\Build-Release.ps1'),
        (Join-Path $repo 'scripts\Test-ReleaseSource.ps1'),
        (Join-Path $repo 'scripts\Test-G32S3RuntimeUi.ps1')
    )
    if (-not [string]::IsNullOrWhiteSpace($FlashScript)) {
        $files += $FlashScript
    }
    if (-not [string]::IsNullOrWhiteSpace($SerialLogScript)) {
        $files += $SerialLogScript
    }
    foreach ($file in $files) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "Missing PowerShell source: $file"
        }
        $tokens = $null
        $parseErrors = $null
        [System.Management.Automation.Language.Parser]::ParseFile(
            $file, [ref]$tokens, [ref]$parseErrors) | Out-Null
        if ($parseErrors.Count -gt 0) {
            $parseErrors | ForEach-Object { Write-Host "STATIC_CHECK_ERROR $file $($_.Message)" }
            throw "PowerShell static check failed: $file"
        }
        Write-Host "STATIC_CHECK_PASS $file"
    }
}

function Assert-BuildInputs {
    if ([string]::IsNullOrWhiteSpace($G32SdkRoot)) {
        throw 'Set G32_SDK_ROOT or pass -G32SdkRoot.'
    }
    if ([string]::IsNullOrWhiteSpace($G32ToolchainBin)) {
        throw 'Set G32_TOOLCHAIN_BIN or pass -G32ToolchainBin.'
    }
}

function Get-InstalledAppSourceRoot {
    $installedApp = "$G32SdkRoot/application/app_tirtc_demo"
    $windowsPath = & wsl.exe -d $Distro -- wslpath -w $installedApp
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($windowsPath)) {
        throw "Unable to map installed APP path from WSL: $installedApp"
    }
    return ($windowsPath | Select-Object -Last 1).Trim()
}

Start-Transcript -Path $sessionTempLog -Force | Out-Null
try {
    if ($Action -in @('Preflight', 'Build', 'Dev')) {
        Write-Host 'Phase 1/2: static checks'
        Test-PowerShellSources
        Assert-BuildInputs
        & (Join-Path $repo 'scripts\Test-ReleaseSource.ps1') -RepoRoot $repo |
            Tee-Object -FilePath $preflightTempLog
        if ($LASTEXITCODE -ne 0) {
            throw "G32 release source check failed with exit code $LASTEXITCODE"
        }

        Write-Host 'Phase 2/2: parameterized SDK validation'
        if ($Action -eq 'Preflight') {
            if ($repo -notmatch '^([A-Za-z]):\\(.*)$') {
                throw "Unsupported Windows repository path: $repo"
            }
            $repoWsl = "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2] -replace '\\', '/')"
            & wsl.exe -d $Distro -- bash "$repoWsl/scripts/build_release.sh" `
                preflight $G32SdkRoot $G32ToolchainBin
            if ($LASTEXITCODE -ne 0) {
                throw "G32 preflight failed with exit code $LASTEXITCODE"
            }
        } else {
            & (Join-Path $repo 'scripts\Build-Release.ps1') `
                -Distro $Distro `
                -G32SdkRoot $G32SdkRoot `
                -G32ToolchainBin $G32ToolchainBin `
                -RepoRoot $repo
            if ($LASTEXITCODE -ne 0) {
                throw "G32 build failed with exit code $LASTEXITCODE"
            }
        }
        & (Join-Path $repo 'scripts\Test-G32S3RuntimeUi.ps1') `
            -SourceRoot (Get-InstalledAppSourceRoot)
        if ($LASTEXITCODE -ne 0) {
            throw "G32 S3 runtime UI check failed with exit code $LASTEXITCODE"
        }
    }
    if ($Action -eq 'Build') { return }

    if ($Action -eq 'BurnMode') {
        & (Join-Path $PSScriptRoot 'Enter-G32BurnMode.ps1') `
            -Port $ComPort -Profile $BootProfile -WaitSeconds $BurnModeWaitSeconds
        return
    }

    if ($Action -eq 'VerifyFlash') {
        & (Join-Path $PSScriptRoot 'Wait-G32FlashResult.ps1') `
            -SessionFile (Join-Path $building 'cloner_session_latest.json') `
            -TimeoutSeconds $BurnModeWaitSeconds
        return
    }

    if ($Action -in @('Flash', 'Dev')) {
        if ([string]::IsNullOrWhiteSpace($FlashScript) -or
            -not (Test-Path -LiteralPath $FlashScript -PathType Leaf)) {
            throw 'Pass -FlashScript or set G32_FLASH_SCRIPT to the board-specific Cloner preparation script.'
        }
        $flashArgs = @{
            Storage='nand'
            ArtifactDir=$building
            ResourceDir=$building
            LogDir=$building
            ComPort=$ComPort
            LogSeconds=$LogSeconds
            BootProfile=$BootProfile
            StopExistingCloner=$true
            OpenCloner=$true
        }
        if ($FirmwareOnly) { $flashArgs.NoResources = $true }
        & $FlashScript @flashArgs
        if ($LASTEXITCODE -ne 0) { throw "G32 flash flow failed with exit code $LASTEXITCODE" }
        Write-Host 'Cloner is open. Click Start before Action BurnMode is run.'
        Write-Host 'After VID_A108 appears, use Action VerifyFlash to require fresh all-policy completion evidence.'
        return
    }

    if ([string]::IsNullOrWhiteSpace($SerialLogScript) -or
        -not (Test-Path -LiteralPath $SerialLogScript -PathType Leaf)) {
        throw 'Pass -SerialLogScript or set G32_SERIAL_LOG_SCRIPT.'
    }
    & $SerialLogScript -Port $ComPort -Seconds $LogSeconds
} finally {
    Stop-Transcript | Out-Null
    Save-SessionLogs
}
