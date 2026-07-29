[CmdletBinding()]
param(
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path $PSScriptRoot -Parent
}
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message
    )

    if ($Text -notmatch $Pattern) {
        throw $Message
    }
}

$required = @(
    "VERSION",
    "app\Makefile",
    "app\include\tirtc_demo_app.h",
    "app\src\tirtc_demo_app.c",
    "app\src\tirtc_demo_cloud.c",
    "app\src\tirtc_demo_media.c",
    "app\ui\ui_tirtc_demo.c",
    "sdk\include\TiRTC\tiRTC.h",
    "sdk\lib\g32\libTiRTC.a",
    "integration\configs\g32s10x_tirtc_app_release_defconfig",
    "integration\package\application\Config.in",
    "integration\package\application\application.mk",
    "integration\package\third_party\tirtc\tirtc.mk",
    "integration\vendor_overrides\application\application.c",
    "integration\vendor_overrides\newlib_riscv\gettimeofday.c"
)

foreach ($relative in $required) {
    $path = Join-Path $RepoRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing release source: $relative"
    }
}

$generated = Get-ChildItem -LiteralPath (Join-Path $RepoRoot "app") -Recurse -File |
    Where-Object { $_.Extension -in @(".o", ".orig") }
if ($generated) {
    throw "Generated files are present in app/: $($generated.FullName -join ', ')"
}

$version = (Get-Content -LiteralPath (Join-Path $RepoRoot "VERSION") -Raw -Encoding UTF8).Trim()
if ($version -ne "0.1.1") {
    throw "Unexpected APP version: $version"
}

$appHeader = Get-Content -LiteralPath (Join-Path $RepoRoot "app\include\tirtc_demo_app.h") -Raw -Encoding UTF8
Assert-Contains $appHeader '#define\s+TIRTC_G32_APP_VERSION\s+"0\.1\.1"' `
    "APP source version does not match VERSION."

$releaseConfigPath = Join-Path $RepoRoot "integration\configs\g32s10x_tirtc_app_release_defconfig"
$releaseConfig = Get-Content -LiteralPath $releaseConfigPath -Raw -Encoding UTF8
foreach ($setting in @(
    "CONFIG_APPLICATION_TIRTC_DEMO=y",
    "CONFIG_TIRTC=y",
    "CONFIG_WIRELESS_ATBM=y",
    "CONFIG_APPLICATION_SERVICES_AUDIO=y",
    "CONFIG_APPLICATION_SERVICES_AUDIO_CAP=y",
    "CONFIG_APPLICATION_SERVICES_AUDIO_PLAYER=y"
)) {
    Assert-Contains $releaseConfig ("(?m)^" + [regex]::Escape($setting) + "$") `
        "Release defconfig is missing $setting"
}
if ($releaseConfig -match '(?m)^CONFIG_APPLICATION_TIRTC_SCREEN_DEBUG=y$') {
    throw "Release defconfig must disable the browser screen debug service."
}

$sdkLibrary = Join-Path $RepoRoot "sdk\lib\g32\libTiRTC.a"
$sdkHash = (Get-FileHash -LiteralPath $sdkLibrary -Algorithm SHA256).Hash.ToLowerInvariant()
$expectedSdkHash = "33e889d70d4459587faf57f611e9648d52accb2bc02d0ec75205a52e9ca27fba"
if ($sdkHash -ne $expectedSdkHash) {
    throw "Unexpected TiRTC SDK library hash: $sdkHash"
}

$credentialPatterns = @(
    '(?i)\b(?:wifi|wlan)[_-]?(?:ssid|password|pass|psk)\b\s*(?:=|:)?\s*["''][^"''<>{}$]{4,}["'']',
    '(?i)\b(?:password|passphrase|secret(?:_key|_key_id)?|access_key(?:_id)?|device_secret(?:_key)?)\b\s*(?:=|:)\s*["''][^"''<>{}$]{4,}["'']',
    '(?i)\b(?:wifi|wlan)[_-]?(?:password|pass|psk)\b\s*=\s*(?![$<])[^\s#;]{8,}',
    ('-{5}BEGIN ' + '[A-Z0-9 ]*PRIVATE KEY-{5}')
)
$binaryExtensions = @(
    ".a", ".bin", ".elf", ".map", ".png", ".jpg", ".jpeg", ".gif",
    ".webp", ".ttf", ".woff", ".zip", ".7z", ".gz"
)
$credentialHits = @()
$trackedFiles = & git -C $RepoRoot ls-files -- .
if ($LASTEXITCODE -ne 0) {
    throw "Unable to enumerate tracked files for the credential scan."
}
foreach ($relative in $trackedFiles) {
    $path = Join-Path $RepoRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        [System.IO.Path]::GetExtension($path).ToLowerInvariant() -in $binaryExtensions) {
        continue
    }
    foreach ($pattern in $credentialPatterns) {
        $credentialHits += Select-String -LiteralPath $path -Pattern $pattern -Encoding UTF8
    }
}
if ($credentialHits) {
    $locations = $credentialHits |
        ForEach-Object { "$($_.Path):$($_.LineNumber)" } |
        Sort-Object -Unique
    throw "Potential plaintext credentials found in tracked text: $($locations -join ', ')"
}

$parseErrors = @()
Get-ChildItem -LiteralPath (Join-Path $RepoRoot "scripts") -Filter *.ps1 -File |
    ForEach-Object {
        $tokens = $null
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile(
            $_.FullName, [ref]$tokens, [ref]$errors
        )
        $parseErrors += $errors
    }
if ($parseErrors.Count -ne 0) {
    throw "PowerShell parse errors: $($parseErrors.Message -join '; ')"
}

Write-Output "G32 APP release source check: PASS"
Write-Output "APP version: $version"
Write-Output "TiRTC SDK: v2.2.1"
Write-Output "libTiRTC.a SHA256: $sdkHash"
Write-Output "Release screen debug: disabled"
Write-Output "Tracked-text credential scan: PASS"
