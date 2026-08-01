[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$ReferenceLocalConfig,
    [string]$FirmwarePath,
    [string]$BuildConfigPath
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Assert-Equal {
    param(
        [string]$Name,
        [string]$Actual,
        [string]$Expected
    )
    if ($Actual.ToLowerInvariant() -ne $Expected.ToLowerInvariant()) {
        $failures.Add("$Name mismatch: actual=$Actual expected=$Expected")
    } else {
        Write-Host "[OK] $Name $Actual"
    }
}

function Assert-NoMatch {
    param(
        [string]$Name,
        [string]$Pattern,
        [string[]]$Paths
    )
    $arguments = @("-n", "--hidden", "--glob", "!components/tirtc_sdk/**",
                   "--glob", "!docs/**", "--glob", "!README.md",
                   "--glob", "!tools/validate_example.ps1", "-e", $Pattern)
    $arguments += $Paths
    $matches = & rg @arguments 2>$null
    if ($LASTEXITCODE -eq 0) {
        $failures.Add("$Name found:`n$($matches -join "`n")")
    } elseif ($LASTEXITCODE -eq 1) {
        Write-Host "[OK] $Name"
    } else {
        throw "rg failed while checking $Name"
    }
}

function Assert-ConfigLine {
    param(
        [string]$Name,
        [string]$Path,
        [string]$Line
    )
    $content = Get-Content -LiteralPath $Path -Encoding UTF8
    if ($content -notcontains $Line) {
        $failures.Add("$Name missing from $Path`: $Line")
    } else {
        Write-Host "[OK] $Name"
    }
}

function Assert-FileNoMatch {
    param(
        [string]$Name,
        [string]$Path,
        [string]$Pattern
    )
    $match = Select-String -LiteralPath $Path -Pattern $Pattern
    if ($null -ne $match) {
        $failures.Add("$Name unexpectedly enabled in $Path")
    } else {
        Write-Host "[OK] $Name"
    }
}

function Assert-NoKnownBaselineSecrets {
    param(
        [string]$ConfigPath,
        [string]$BinaryPath
    )
    $config = Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $binaryText = $latin1.GetString(
        [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $BinaryPath).Path)
    )
    $names = @(
        "APP_WIFI_SSID",
        "APP_WIFI_PASSWORD",
        "TIRTC_DEVICE_ID",
        "TIRTC_DEVICE_SECRET_KEY",
        "TIRTC_REMOTE_DEVICE_ID"
    )
    foreach ($name in $names) {
        $pattern = "(?m)^\s*#define\s+" +
                   [regex]::Escape($name) +
                   "\s+`"([^`"]*)`""
        $match = [regex]::Match($config, $pattern)
        if (-not $match.Success) {
            $failures.Add("known-secret macro missing from reference: $name")
            continue
        }
        $valueBytes = [Text.Encoding]::UTF8.GetBytes($match.Groups[1].Value)
        if ($valueBytes.Length -lt 8) {
            Write-Host "[SKIP] $name is shorter than 8 bytes; review manually"
            continue
        }
        $needle = $latin1.GetString($valueBytes)
        if ($binaryText.IndexOf(
                $needle,
                [StringComparison]::Ordinal
            ) -ge 0) {
            $failures.Add("known baseline value embedded in firmware: $name")
        } else {
            Write-Host "[OK] firmware excludes known $name value"
        }
    }
}

$sdk = Join-Path $ProjectRoot "components\tirtc_sdk\lib\esp32s3\libTiRTC.a"
$audio = Join-Path $ProjectRoot "media\audio_g711a_8khz_mono_20ms_2s_100packets.g711a"
$video = Join-Path $ProjectRoot "media\video_h264_annexb_640x480_15fps_8s_120frames.h264"
$promptAssets = @(
    @{
        Name = "AI story prompt"
        Path = Join-Path $ProjectRoot "media\ai_story.g711a"
        Hash = "67381051a72ba8e29822d1f2267a7c5115fa849948535939a59d4fae479dbffd"
    },
    @{
        Name = "AI joke prompt"
        Path = Join-Path $ProjectRoot "media\ai_joke.g711a"
        Hash = "a7bb51c454b5e4c1251502bf8d3ab6d974b8139205f4130599053df980bec888"
    },
    @{
        Name = "AI weather prompt"
        Path = Join-Path $ProjectRoot "media\ai_weather.g711a"
        Hash = "8d080adb9b1b83dd02fab2c8f41ee8534aacb13225cffc71a966b07aa467df63"
    },
    @{
        Name = "AI call Xiaozhang prompt"
        Path = Join-Path $ProjectRoot "media\ai_call_xiaozhang.g711a"
        Hash = "530c6041bd4b51c24176ffa1c0434954687f2f18bb0810a496d05ba18859173c"
    },
    @{
        Name = "AI call Xiaoli prompt"
        Path = Join-Path $ProjectRoot "media\ai_call_xiaoli.g711a"
        Hash = "730c29cd4dbf4504a2ee8bdb8742e6a1d296794811115235727ed6680a35a480"
    }
)

Assert-Equal "TiRTC SDK SHA256" `
    (Get-FileHash -Algorithm SHA256 $sdk).Hash `
    "dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e"
Assert-Equal "G711A media SHA256" `
    (Get-FileHash -Algorithm SHA256 $audio).Hash `
    "60fa7335ca0aebddc119476c0b46b3faf047affd5a7c5cb24b0199fdf845956f"
Assert-Equal "H264 media SHA256" `
    (Get-FileHash -Algorithm SHA256 $video).Hash `
    "27735e98fcab14ddd916d6dd0deb86a4fa5d2b04472605234115a681db67b8d8"
foreach ($asset in $promptAssets) {
    Assert-Equal "$($asset.Name) SHA256" `
        (Get-FileHash -Algorithm SHA256 $asset.Path).Hash `
        $asset.Hash
    $length = (Get-Item -LiteralPath $asset.Path).Length
    if ($length -le 0 -or ($length % 160) -ne 0) {
        $failures.Add(
            "$($asset.Name) must be non-empty and 160-byte packet aligned"
        )
    } else {
        Write-Host "[OK] $($asset.Name) packet alignment $length bytes"
    }
}

$defaults = Join-Path $ProjectRoot "sdkconfig.defaults"
Assert-ConfigLine "default MQTT v5 support" $defaults "CONFIG_MQTT_PROTOCOL_5=y"
Assert-ConfigLine "default FreeRTOS 1000 Hz" $defaults "CONFIG_FREERTOS_HZ=1000"
Assert-ConfigLine "default lwIP socket budget" $defaults `
    "CONFIG_LWIP_MAX_SOCKETS=16"
Assert-ConfigLine "default native USB AT isolation" $defaults `
    "# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set"
Assert-ConfigLine "default UART0 diagnostics" $defaults `
    "CONFIG_ESP_CONSOLE_UART_DEFAULT=y"
Assert-ConfigLine "default quiet bootloader logs" $defaults `
    "CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y"
Assert-ConfigLine "default quiet application logs" $defaults `
    "CONFIG_LOG_DEFAULT_LEVEL_WARN=y"
Assert-ConfigLine "default Chinese INFO log ceiling" $defaults `
    "CONFIG_LOG_MAXIMUM_LEVEL_INFO=y"
Assert-ConfigLine "default dynamic log control" $defaults `
    "CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=y"
Assert-ConfigLine "default balanced mbedTLS allocator" $defaults `
    "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y"
Assert-ConfigLine "default PSRAM malloc integration" $defaults `
    "CONFIG_SPIRAM_USE_MALLOC=y"
Assert-ConfigLine "default PSRAM internal threshold" $defaults `
    "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512"
Assert-ConfigLine "default internal heap reserve" $defaults `
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=98304"
Assert-ConfigLine "default external task stacks" $defaults `
    "CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y"
Assert-ConfigLine "default certificate bundle" $defaults `
    "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y"
$mainCmake = Join-Path $ProjectRoot "main\CMakeLists.txt"
Assert-ConfigLine "TiRTC raw peer-state filter source" $mainCmake `
    '        "app_stdio_filter.c"'
Assert-ConfigLine "TiRTC raw peer-state log filter" $mainCmake `
    'target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=printf")'
$stdioFilter = Join-Path $ProjectRoot "main\app_stdio_filter.c"
Assert-ConfigLine "TiRTC exact peer-state trace match" $stdioFilter `
    '    "peer_connection state change e=%d\n";'
Assert-ConfigLine "TiRTC peer-state trace comparison" $stdioFilter `
    "    if (format != NULL && strcmp(format, TIRTC_PEER_STATE_TRACE) == 0) {"
Assert-ConfigLine "TiRTC raw console marker" $stdioFilter `
    'static const char *TIRTC_CONSOLE_LOG_MARKER = "] [tiRTC]  ";'
Assert-ConfigLine "TiRTC raw console prefix" $stdioFilter `
    '        strncmp(line, "\x1b[1;3", 5) != 0 ||'
Assert-ConfigLine "TiRTC raw console color range" $stdioFilter `
    "    return line[5] >= '1' &&"
Assert-ConfigLine "TiRTC raw console comparison" $stdioFilter `
    "           strstr(line, TIRTC_CONSOLE_LOG_MARKER) != NULL;"
Assert-ConfigLine "unrelated printf forwarding" $stdioFilter `
    "    int written = vprintf(format, arguments);"

$hasExplicitBuildConfig =
    -not [string]::IsNullOrWhiteSpace($BuildConfigPath)
$configuredSdkconfig = if (-not $hasExplicitBuildConfig) {
    Join-Path $ProjectRoot "sdkconfig"
} else {
    $BuildConfigPath
}
if (Test-Path -LiteralPath $configuredSdkconfig) {
    $configuredSdkconfig =
        (Resolve-Path -LiteralPath $configuredSdkconfig).Path
    $canonicalSdkconfig =
        (Resolve-Path -LiteralPath (Join-Path $ProjectRoot "sdkconfig")).Path
    Assert-Equal "configured sdkconfig ownership" `
        $configuredSdkconfig $canonicalSdkconfig
    Assert-ConfigLine "configured MQTT v5 support" $configuredSdkconfig `
        "CONFIG_MQTT_PROTOCOL_5=y"
    Assert-ConfigLine "configured FreeRTOS 1000 Hz" $configuredSdkconfig `
        "CONFIG_FREERTOS_HZ=1000"
    Assert-ConfigLine "configured lwIP socket budget" $configuredSdkconfig `
        "CONFIG_LWIP_MAX_SOCKETS=16"
    Assert-ConfigLine "configured UART0 diagnostics" $configuredSdkconfig `
        "CONFIG_ESP_CONSOLE_UART_DEFAULT=y"
    Assert-ConfigLine "configured native USB console disabled" `
        $configuredSdkconfig `
        "# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set"
    Assert-ConfigLine "configured secondary console disabled" `
        $configuredSdkconfig `
        "CONFIG_ESP_CONSOLE_SECONDARY_NONE=y"
    Assert-ConfigLine "configured quiet bootloader logs" `
        $configuredSdkconfig `
        "CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y"
    Assert-ConfigLine "configured quiet application logs" `
        $configuredSdkconfig `
        "CONFIG_LOG_DEFAULT_LEVEL_WARN=y"
    Assert-ConfigLine "configured Chinese INFO log ceiling" `
        $configuredSdkconfig `
        "CONFIG_LOG_MAXIMUM_LEVEL_INFO=y"
    Assert-ConfigLine "configured dynamic log control" `
        $configuredSdkconfig `
        "CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=y"
    Assert-ConfigLine "configured 16 MB flash" $configuredSdkconfig `
        "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y"
    Assert-ConfigLine "configured octal PSRAM" $configuredSdkconfig `
        "CONFIG_SPIRAM_MODE_OCT=y"
    Assert-ConfigLine "configured FreeRTOS trace disabled" `
        $configuredSdkconfig `
        "# CONFIG_FREERTOS_USE_TRACE_FACILITY is not set"
    Assert-ConfigLine "configured runtime stats disabled" `
        $configuredSdkconfig `
        "# CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is not set"
    Assert-ConfigLine "configured balanced mbedTLS allocator" `
        $configuredSdkconfig `
        "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y"
    Assert-ConfigLine "configured PSRAM malloc integration" `
        $configuredSdkconfig "CONFIG_SPIRAM_USE_MALLOC=y"
    Assert-ConfigLine "configured PSRAM internal threshold" `
        $configuredSdkconfig "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512"
    Assert-ConfigLine "configured internal heap reserve" `
        $configuredSdkconfig "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=98304"
    Assert-ConfigLine "configured external task stacks" `
        $configuredSdkconfig "CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y"
    Assert-ConfigLine "configured certificate bundle" `
        $configuredSdkconfig "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y"
} elseif ($hasExplicitBuildConfig) {
    $failures.Add("configured sdkconfig not found: $configuredSdkconfig")
} else {
    Write-Host "[INFO] generated sdkconfig not present; defaults-only check"
}

$hasReference = -not [string]::IsNullOrWhiteSpace($ReferenceLocalConfig)
$hasFirmware = -not [string]::IsNullOrWhiteSpace($FirmwarePath)
if ($hasReference -xor $hasFirmware) {
    $failures.Add(
        "ReferenceLocalConfig and FirmwarePath must be supplied together"
    )
} elseif ($hasReference) {
    Assert-NoKnownBaselineSecrets $ReferenceLocalConfig $FirmwarePath
}
if ($hasFirmware -and (Test-Path -LiteralPath $FirmwarePath)) {
    $firmware = (Resolve-Path -LiteralPath $FirmwarePath).Path
    $firmwareBuildDir = Split-Path -Parent $firmware
    $generatedConfig =
        Join-Path $firmwareBuildDir "config\sdkconfig.h"
    $descriptionPath =
        Join-Path $firmwareBuildDir "project_description.json"
    if (Test-Path -LiteralPath $generatedConfig) {
        Assert-ConfigLine "firmware MQTT v5 support" $generatedConfig `
            "#define CONFIG_MQTT_PROTOCOL_5 1"
        Assert-ConfigLine "firmware FreeRTOS 1000 Hz" $generatedConfig `
            "#define CONFIG_FREERTOS_HZ 1000"
        Assert-ConfigLine "firmware lwIP socket budget" $generatedConfig `
            "#define CONFIG_LWIP_MAX_SOCKETS 16"
        Assert-ConfigLine "firmware UART0 diagnostics" $generatedConfig `
            "#define CONFIG_ESP_CONSOLE_UART_DEFAULT 1"
        Assert-ConfigLine "firmware secondary console disabled" `
            $generatedConfig `
            "#define CONFIG_ESP_CONSOLE_SECONDARY_NONE 1"
        Assert-ConfigLine "firmware quiet bootloader logs" `
            $generatedConfig `
            "#define CONFIG_BOOTLOADER_LOG_LEVEL_WARN 1"
        Assert-ConfigLine "firmware quiet application logs" `
            $generatedConfig `
            "#define CONFIG_LOG_DEFAULT_LEVEL_WARN 1"
        Assert-ConfigLine "firmware Chinese INFO log ceiling" `
            $generatedConfig `
            "#define CONFIG_LOG_MAXIMUM_LEVEL_INFO 1"
        Assert-ConfigLine "firmware dynamic log control" `
            $generatedConfig `
            "#define CONFIG_LOG_DYNAMIC_LEVEL_CONTROL 1"
        Assert-ConfigLine "firmware 16 MB flash" $generatedConfig `
            "#define CONFIG_ESPTOOLPY_FLASHSIZE_16MB 1"
        Assert-ConfigLine "firmware octal PSRAM" $generatedConfig `
            "#define CONFIG_SPIRAM_MODE_OCT 1"
        Assert-ConfigLine "firmware balanced mbedTLS allocator" `
            $generatedConfig `
            "#define CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC 1"
        Assert-ConfigLine "firmware PSRAM malloc integration" `
            $generatedConfig "#define CONFIG_SPIRAM_USE_MALLOC 1"
        Assert-ConfigLine "firmware PSRAM internal threshold" `
            $generatedConfig `
            "#define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 512"
        Assert-ConfigLine "firmware internal heap reserve" `
            $generatedConfig `
            "#define CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL 98304"
        Assert-ConfigLine "firmware external task stacks" `
            $generatedConfig `
            "#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1"
        Assert-ConfigLine "firmware certificate bundle" `
            $generatedConfig "#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 1"
        Assert-FileNoMatch "firmware FreeRTOS trace disabled" `
            $generatedConfig `
            "^#define CONFIG_FREERTOS_USE_TRACE_FACILITY "
        Assert-FileNoMatch "firmware runtime stats disabled" `
            $generatedConfig `
            "^#define CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS "
        $firmwareTime = (Get-Item -LiteralPath $firmware).LastWriteTimeUtc
        $configTime =
            (Get-Item -LiteralPath $generatedConfig).LastWriteTimeUtc
        if ($firmwareTime -lt $configTime) {
            $failures.Add(
                "firmware predates generated sdkconfig.h in $firmwareBuildDir"
            )
        } else {
            Write-Host "[OK] firmware is not older than generated config"
        }
        $firmwareInputs = @()
        foreach ($sourceRoot in @("main", "components")) {
            $firmwareInputs += Get-ChildItem -LiteralPath (
                Join-Path $ProjectRoot $sourceRoot
            ) -Recurse -File
        }
        foreach ($sourceFile in @(
            "CMakeLists.txt",
            "sdkconfig",
            "sdkconfig.defaults",
            "partitions.csv"
        )) {
            $firmwareInputs += Get-Item -LiteralPath (
                Join-Path $ProjectRoot $sourceFile
            )
        }
        $newestFirmwareInput = $firmwareInputs |
            Sort-Object LastWriteTimeUtc -Descending |
            Select-Object -First 1
        if ($firmwareTime -lt $newestFirmwareInput.LastWriteTimeUtc) {
            $failures.Add(
                "firmware predates current source/config: " +
                $newestFirmwareInput.FullName
            )
        } else {
            Write-Host "[OK] firmware is not older than source/config"
        }
    } else {
        $failures.Add(
            "generated firmware config not found: $generatedConfig"
        )
    }
    if (Test-Path -LiteralPath $descriptionPath) {
        $description =
            Get-Content -LiteralPath $descriptionPath -Raw -Encoding UTF8 |
            ConvertFrom-Json
        $describedBuildDir =
            (Resolve-Path -LiteralPath $description.build_dir).Path
        $describedConfig =
            (Resolve-Path -LiteralPath $description.config_file).Path
        $describedProject =
            (Resolve-Path -LiteralPath $description.project_path).Path
        [string[]]$describedDefaults = @(
            @($description.config_defaults) |
                ForEach-Object { (Resolve-Path -LiteralPath $_).Path }
        )
        $canonicalDefaults =
            (Resolve-Path -LiteralPath (
                Join-Path $ProjectRoot "sdkconfig.defaults"
            )).Path
        $describedFirmware =
            Join-Path $describedBuildDir $description.app_bin
        $describedElf =
            Join-Path $describedBuildDir $description.app_elf
        Assert-Equal "firmware build directory" `
            $firmwareBuildDir $describedBuildDir
        Assert-Equal "firmware project ownership" `
            $describedProject $ProjectRoot
        Assert-Equal "firmware configured sdkconfig" `
            $describedConfig $configuredSdkconfig
        if ($describedDefaults.Count -ne 1) {
            $failures.Add(
                "firmware config defaults count is not one: " +
                "$($describedDefaults.Count)"
            )
        } else {
            Assert-Equal "firmware config defaults ownership" `
                $describedDefaults[0] $canonicalDefaults
        }
        Assert-Equal "firmware path from build description" `
            $firmware $describedFirmware
        Assert-Equal "firmware project name" `
            ([string]$description.project_name) `
            "tirtc_esp32s3_at_thingconnect_demo"
        Assert-Equal "firmware target" ([string]$description.target) "esp32s3"
        Assert-Equal "firmware ESP-IDF" `
            ([string]$description.git_revision) "v5.5.4"
        if (Test-Path -LiteralPath $describedElf) {
            Write-Host "[INFO] app ELF SHA256 " `
                (Get-FileHash -Algorithm SHA256 $describedElf).Hash
        } else {
            $failures.Add("firmware ELF not found: $describedElf")
        }
        $requiredArtifacts = @(
            (Join-Path $firmwareBuildDir "bootloader\bootloader.bin"),
            (Join-Path $firmwareBuildDir "partition_table\partition-table.bin"),
            (Join-Path $firmwareBuildDir "ota_data_initial.bin"),
            (Join-Path $firmwareBuildDir "storage.bin"),
            (Join-Path $firmwareBuildDir "flasher_args.json")
        )
        foreach ($artifact in $requiredArtifacts) {
            if (-not (Test-Path -LiteralPath $artifact)) {
                $failures.Add("required flash artifact not found: $artifact")
            } else {
                Write-Host "[OK] flash artifact " `
                    (Split-Path -Leaf $artifact)
            }
        }
        $storage = Join-Path $firmwareBuildDir "storage.bin"
        if (Test-Path -LiteralPath $storage) {
            $newestMedia = Get-ChildItem -LiteralPath (
                Join-Path $ProjectRoot "media"
            ) -File |
                Sort-Object LastWriteTimeUtc -Descending |
                Select-Object -First 1
            if ((Get-Item -LiteralPath $storage).LastWriteTimeUtc -lt
                $newestMedia.LastWriteTimeUtc) {
                $failures.Add("storage.bin predates recorded media")
            } else {
                Write-Host "[OK] storage image is not older than media"
            }
        }
    } else {
        $failures.Add(
            "firmware build description not found: $descriptionPath"
        )
    }
    Write-Host "[INFO] firmware SHA256 " `
        (Get-FileHash -Algorithm SHA256 $firmware).Hash
}

Push-Location $ProjectRoot
try {
    Assert-NoMatch "application VoIP symbols" `
        "(PLATFORM_SERVICE_VOIP|DEVICE_SERVICE_VOIP|session_runtime_voip|wechat_voip)" `
        @("main", "components")
    Assert-NoMatch "local token generator or static AK/SK" `
        "(tirtc_token|ACCESS_KEY|AK_ID|AK_SECRET)" `
        @("main", "components")
    Assert-NoMatch "compile-time Wi-Fi credentials" `
        "(WIFI_SSID|WIFI_PASSWORD|CONFIG_ESP_WIFI_SSID|CONFIG_ESP_WIFI_PASSWORD)" `
        @("main", "components")
    Assert-NoMatch "SoftAP or web provisioning" `
        "(WIFI_MODE_AP|esp_http_server|httpd_start|softap)" `
        @("main", "components\wifi_manager")
} finally {
    Pop-Location
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Validation failed:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "- $_" -ForegroundColor Red }
    exit 1
}

Write-Host ""
Write-Host "Static validation passed." -ForegroundColor Green
exit 0
