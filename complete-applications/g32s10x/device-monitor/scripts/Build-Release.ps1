[CmdletBinding()]
param(
    [string]$Distro = "Ubuntu",
    [string]$G32SdkRoot = $env:G32_SDK_ROOT,
    [string]$G32ToolchainBin = $env:G32_TOOLCHAIN_BIN,
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($G32SdkRoot)) {
    throw "Set G32_SDK_ROOT or pass -G32SdkRoot with the WSL path to im_sdk/opensource/freertos."
}
if ([string]::IsNullOrWhiteSpace($G32ToolchainBin)) {
    throw "Set G32_TOOLCHAIN_BIN or pass -G32ToolchainBin with the WSL toolchain bin path."
}
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path $PSScriptRoot -Parent
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

& (Join-Path $PSScriptRoot "Test-ReleaseSource.ps1") -RepoRoot $RepoRoot

if ($RepoRoot -notmatch '^([A-Za-z]):\\(.*)$') {
    throw "Unsupported Windows repository path: $RepoRoot"
}
$repoWsl = "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2] -replace '\\', '/')"

& wsl.exe -d $Distro -- bash "$repoWsl/scripts/build_release.sh" `
    preflight $G32SdkRoot $G32ToolchainBin
if ($LASTEXITCODE -ne 0) {
    throw "WSL preflight failed."
}

& wsl.exe -d $Distro -u root -- bash "$repoWsl/scripts/build_release.sh" `
    clean $G32SdkRoot $G32ToolchainBin
if ($LASTEXITCODE -ne 0) {
    throw "SDK clean failed."
}

& wsl.exe -d $Distro -- bash "$repoWsl/scripts/build_release.sh" `
    firmware $G32SdkRoot $G32ToolchainBin
if ($LASTEXITCODE -ne 0) {
    throw "Firmware build failed."
}

& wsl.exe -d $Distro -u root -- bash "$repoWsl/scripts/build_release.sh" `
    filesystems $G32SdkRoot $G32ToolchainBin
if ($LASTEXITCODE -ne 0) {
    throw "YAFFS build failed."
}

& wsl.exe -d $Distro -- bash "$repoWsl/scripts/build_release.sh" `
    collect $G32SdkRoot $G32ToolchainBin
if ($LASTEXITCODE -ne 0) {
    throw "Artifact collection failed."
}

[Console]::Beep(1200, 3000)
Write-Output "G32 APP v0.1.1 release build: PASS"
Write-Output "Artifacts: $(Join-Path $RepoRoot 'building')"
