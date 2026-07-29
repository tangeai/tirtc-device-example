[CmdletBinding()]
param(
    [string]$SourceRoot = $env:G32_APP_SOURCE_ROOT
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    throw "Set G32_APP_SOURCE_ROOT or pass -SourceRoot with the installed app_tirtc_demo path."
}

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

function Assert-NotContains {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message
    )

    if ($Text -match $Pattern) {
        throw $Message
    }
}

$uiPath = Join-Path $SourceRoot "ui\ui_tirtc_demo.c"
$pixelPath = Join-Path $SourceRoot "ui\ui_tirtc_demo_pixel.h"
$assetPath = Join-Path $SourceRoot "ui\tirtc_demo_embedded_assets.c"
$baseTextAssetPath = Join-Path $SourceRoot "ui\text_assets.c"
$extraTextAssetPath = Join-Path $SourceRoot "ui\tirtc_demo_s3_text_extra.c"
$makefilePath = Join-Path $SourceRoot "Makefile"
$appHeaderPath = Join-Path $SourceRoot "include\tirtc_demo_app.h"
$appSourcePath = Join-Path $SourceRoot "src\tirtc_demo_app.c"
$cloudSourcePath = Join-Path $SourceRoot "src\tirtc_demo_cloud.c"
$avatarSourcePath = Join-Path $SourceRoot "ui\ai_chat_avatar_assets.c"
$avatarHeaderPath = Join-Path $SourceRoot "ui\ai_chat_avatar_assets.h"
$freeRtosRoot = Split-Path (Split-Path $SourceRoot -Parent) -Parent
$lvConfPath = Join-Path $freeRtosRoot "third_party\lvgl\lv_conf.h"
$gettimeofdayPath = Join-Path $freeRtosRoot "newlib_riscv\gettimeofday.c"
$posixClockPath = Join-Path $freeRtosRoot "os\posix\clock.c"
$mbedtlsConfigPath = Join-Path $freeRtosRoot "third_party\mbedtls\mbedtls_code\include\mbedtls\config.h"
$mbedtlsMakefilePath = Join-Path $freeRtosRoot "third_party\mbedtls\Makefile"
$altcpTlsPath = Join-Path $freeRtosRoot "net\lwip\apps\altcp_tls\altcp_tls_mbedtls.c"
$ecdhPath = Join-Path $freeRtosRoot "third_party\mbedtls\mbedtls_code\library\ecdh.c"
$atbmMakefilePath = Join-Path $freeRtosRoot "devices\wireless\atbm\Makefile"
$atbmEcpPath = Join-Path $freeRtosRoot "devices\wireless\atbm\net\wpa\sae\ecp.c"
$atbmEcpExtendPath = Join-Path $freeRtosRoot "devices\wireless\atbm\net\wpa\sae\ecp_extend.c"
$atbmThreadPath = Join-Path $freeRtosRoot "devices\wireless\atbm\os\Ingenic\atbm_os_thread.c"
$sslClientPath = Join-Path $freeRtosRoot "third_party\mbedtls\mbedtls_code\library\ssl_cli.c"
$lwipOptionsPath = Join-Path $freeRtosRoot "net\lwip\include\lwipopts.h"
$tirtcHeaderPath = Join-Path $freeRtosRoot "third_party\tirtc\include\TiRTC\tiRTC.h"
$tirtcLibraryPath = Join-Path $freeRtosRoot "third_party\tirtc\lib\g32\libTiRTC.a"

foreach ($path in @(
    $uiPath,
    $pixelPath,
    $assetPath,
    $baseTextAssetPath,
    $extraTextAssetPath,
    $makefilePath,
    $appHeaderPath,
    $appSourcePath,
    $cloudSourcePath,
    $avatarSourcePath,
    $avatarHeaderPath,
    $lvConfPath,
    $gettimeofdayPath,
    $posixClockPath,
    $mbedtlsConfigPath,
    $mbedtlsMakefilePath,
    $altcpTlsPath,
    $ecdhPath,
    $atbmMakefilePath,
    $atbmEcpPath,
    $atbmEcpExtendPath,
    $atbmThreadPath,
    $sslClientPath,
    $lwipOptionsPath,
    $tirtcHeaderPath,
    $tirtcLibraryPath
)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing source file: $path"
    }
}

$ui = Get-Content -LiteralPath $uiPath -Raw -Encoding UTF8
$pixel = Get-Content -LiteralPath $pixelPath -Raw -Encoding UTF8
$assets = Get-Content -LiteralPath $assetPath -Raw -Encoding UTF8
$baseTextAssets = Get-Content -LiteralPath $baseTextAssetPath -Raw -Encoding UTF8
$extraTextAssets = Get-Content -LiteralPath $extraTextAssetPath -Raw -Encoding UTF8
$makefile = Get-Content -LiteralPath $makefilePath -Raw -Encoding UTF8
$appHeader = Get-Content -LiteralPath $appHeaderPath -Raw -Encoding UTF8
$appSource = Get-Content -LiteralPath $appSourcePath -Raw -Encoding UTF8
$cloudSource = Get-Content -LiteralPath $cloudSourcePath -Raw -Encoding UTF8
$avatarSource = Get-Content -LiteralPath $avatarSourcePath -Raw -Encoding UTF8
$avatarHeader = Get-Content -LiteralPath $avatarHeaderPath -Raw -Encoding UTF8
$lvConf = Get-Content -LiteralPath $lvConfPath -Raw -Encoding UTF8
$gettimeofday = Get-Content -LiteralPath $gettimeofdayPath -Raw -Encoding UTF8
$posixClock = Get-Content -LiteralPath $posixClockPath -Raw -Encoding UTF8
$mbedtlsConfig = Get-Content -LiteralPath $mbedtlsConfigPath -Raw -Encoding UTF8
$mbedtlsMakefile = Get-Content -LiteralPath $mbedtlsMakefilePath -Raw -Encoding UTF8
$altcpTls = Get-Content -LiteralPath $altcpTlsPath -Raw -Encoding UTF8
$ecdh = Get-Content -LiteralPath $ecdhPath -Raw -Encoding UTF8
$atbmMakefile = Get-Content -LiteralPath $atbmMakefilePath -Raw -Encoding UTF8
$atbmEcp = Get-Content -LiteralPath $atbmEcpPath -Raw -Encoding UTF8
$atbmEcpExtend = Get-Content -LiteralPath $atbmEcpExtendPath -Raw -Encoding UTF8
$atbmThread = Get-Content -LiteralPath $atbmThreadPath -Raw -Encoding UTF8
$sslClient = Get-Content -LiteralPath $sslClientPath -Raw -Encoding UTF8
$lwipOptions = Get-Content -LiteralPath $lwipOptionsPath -Raw -Encoding UTF8
$tirtcHeader = Get-Content -LiteralPath $tirtcHeaderPath -Raw -Encoding UTF8
$runtimeUi = $ui + "`n" + $pixel

Assert-Contains $ui '#define\s+TIRTC_DEMO_USE_S3_PIXEL_UI\s+1' `
    "S3 320x240 runtime UI is not enabled."
Assert-Contains $ui '#define\s+TIRTC_DEMO_USE_S3_TEXT_IMAGES\s+1' `
    "S3 fixed-copy image lookup is not enabled."
Assert-NotContains $ui 'TIRTC_DEMO_USE_FIGMA_SCREEN_IMAGES' `
    "Legacy full-screen image rendering switch is still present."
Assert-NotContains $pixel 'tirtc_demo_pixel_reference|figma_screen_|240617000001|oWX2406170001' `
    "Legacy screenshot shell or fake test identities remain in runtime UI code."
Assert-NotContains $pixel 'OTA|关于 / OTA' `
    "OTA must not be present in the G32 product UI."
Assert-Contains $makefile 'ui/text_assets\.c' `
    "S3 fixed-copy image assets are not compiled."
Assert-Contains $makefile 'ui/tirtc_demo_s3_text_extra\.c' `
    "G32 supplemental fixed-copy image assets are not compiled."
Assert-Contains $makefile 'ui/ai_chat_avatar_assets\.c' `
    "Exact S3 AI avatar states are not compiled."
Assert-NotContains $assets '\{"figma_screen_[0-9]{2}\.png"' `
    "Full-screen images are still registered as runtime resources."

$assetCount = [regex]::Matches(
    $assets,
    '(?m)^\s*\{"(?:home_|call_contacts_|ai_chat_)[^"]+\.png",'
).Count
if ($assetCount -ne 20) {
    throw "Expected 20 component image assets, found $assetCount."
}

$fixedCjkStrings = [regex]::Matches(
    $pixel,
    '"(?:\\.|[^"\\])*[\u4e00-\u9fff](?:\\.|[^"\\])*"'
) | ForEach-Object { $_.Value.Trim('"') } | Sort-Object -Unique
$missingFixedCopy = @(
    foreach ($copy in $fixedCjkStrings) {
        $escapedCopy = [regex]::Escape($copy)
        $registration = '\{"' + $escapedCopy + '",\s*(?:10|12|16),'
        if ($baseTextAssets -notmatch $registration -and
            $extraTextAssets -notmatch $registration) {
            $copy
        }
    }
)
if ($missingFixedCopy.Count -gt 0) {
    throw "Missing fixed-copy image assets: $($missingFixedCopy -join ', ')"
}

$requiredFunctions = @(
    'tirtc_demo_pixel_create_home',
    'tirtc_demo_pixel_create_detail',
    'tirtc_demo_pixel_call_home',
    'tirtc_demo_pixel_create_add_contact',
    'tirtc_demo_pixel_create_add_contact_edit',
    'tirtc_demo_pixel_create_scan_info',
    'tirtc_demo_pixel_create_contacts',
    'tirtc_demo_pixel_create_active_call',
    'tirtc_demo_pixel_create_ai',
    'tirtc_demo_pixel_create_ai_settings',
    'tirtc_demo_pixel_create_system',
    'tirtc_demo_pixel_create_config',
    'tirtc_demo_pixel_create_diagnostics',
    'tirtc_demo_pixel_create_tirtc_test'
)
foreach ($functionName in $requiredFunctions) {
    Assert-Contains $pixel ([regex]::Escape($functionName) + '\s*\(') `
        "Missing runtime UI function: $functionName"
}

$requiredViews = @(
    'TIRTC_DEMO_VIEW_HOME',
    'TIRTC_DEMO_VIEW_DETAIL',
    'TIRTC_DEMO_VIEW_CALL',
    'TIRTC_DEMO_VIEW_ACTIVE_CALL',
    'TIRTC_DEMO_VIEW_CALL_INCOMING',
    'TIRTC_DEMO_VIEW_ADD_CONTACT',
    'TIRTC_DEMO_VIEW_CALL_ADD_EDIT',
    'TIRTC_DEMO_VIEW_CALL_SCAN_INFO',
    'TIRTC_DEMO_VIEW_CONTACTS',
    'TIRTC_DEMO_VIEW_WECHAT',
    'TIRTC_DEMO_VIEW_WECHAT_ADD',
    'TIRTC_DEMO_VIEW_WECHAT_ADD_EDIT',
    'TIRTC_DEMO_VIEW_WECHAT_SCAN_INFO',
    'TIRTC_DEMO_VIEW_WECHAT_CONTACTS',
    'TIRTC_DEMO_VIEW_WECHAT_ACTIVE',
    'TIRTC_DEMO_VIEW_WECHAT_INCOMING',
    'TIRTC_DEMO_VIEW_AI_TALK',
    'TIRTC_DEMO_VIEW_AI_SETTINGS',
    'TIRTC_DEMO_VIEW_SYSTEM',
    'TIRTC_DEMO_VIEW_CONFIG',
    'TIRTC_DEMO_VIEW_CONFIG_EDIT',
    'TIRTC_DEMO_VIEW_DEVICE_INFO',
    'TIRTC_DEMO_VIEW_DEVICE_QR',
    'TIRTC_DEMO_VIEW_DIAGNOSTICS',
    'TIRTC_DEMO_VIEW_TIRTC_TEST'
)
foreach ($viewName in $requiredViews) {
    Assert-Contains $pixel ("case\s+" + [regex]::Escape($viewName) + "\s*:") `
        "Missing runtime view mapping: $viewName"
}

$requiredApiBindings = @(
    'tirtc_demo_app_enter',
    'tirtc_demo_app_exit',
    'tirtc_demo_app_restart',
    'tirtc_demo_app_refresh_network',
    'tirtc_demo_app_request_binding',
    'tirtc_demo_app_reset_binding',
    'tirtc_demo_app_run_diagnostics',
    'tirtc_demo_app_refresh_contacts',
    'tirtc_demo_app_request_contact',
    'tirtc_demo_app_adjust_volume',
    'tirtc_demo_app_toggle_mute',
    'tirtc_demo_app_call_contact',
    'tirtc_demo_app_handle_call',
    'tirtc_demo_app_handle_ai',
    'tirtc_demo_app_set_ai_avatar',
    'tirtc_demo_app_refresh_wechat',
    'tirtc_demo_app_call_wechat',
    'tirtc_demo_app_add_wechat_contact',
    'tirtc_demo_app_delete_wechat_contact',
    'tirtc_demo_app_accept_wechat',
    'tirtc_demo_app_reject_wechat',
    'tirtc_demo_app_prepare_external_media',
    'tirtc_demo_app_get_status',
    'tirtc_demo_app_get_runtime',
    'tirtc_demo_app_get_settings',
    'tirtc_demo_app_get_contacts',
    'tirtc_demo_app_get_wechat_contacts',
    'tirtc_demo_app_copy_ai_messages',
    'tirtc_demo_app_save_config'
)
foreach ($apiName in $requiredApiBindings) {
    Assert-Contains $appHeader ([regex]::Escape($apiName) + '\s*\(') `
        "Missing application API declaration: $apiName"
    Assert-Contains $runtimeUi ([regex]::Escape($apiName) + '\s*\(') `
        "Runtime UI is not bound to application API: $apiName"
    Assert-Contains $appSource `
        ('(?m)^\s*(?:void|int|size_t)\s+' + [regex]::Escape($apiName) + '\s*\(') `
        "Application API has no implementation: $apiName"
}

Assert-Contains $avatarHeader '#define\s+AI_CHAT_AVATAR_ASSET_WIDTH\s+96' `
    "S3 AI avatar width is not 96 pixels."
Assert-Contains $avatarHeader '#define\s+AI_CHAT_AVATAR_ASSET_HEIGHT\s+96' `
    "S3 AI avatar height is not 96 pixels."
if ([regex]::Matches($avatarSource, '(?m)^const lv_img_dsc_t ai_chat_avatar_').Count -ne 12) {
    throw "Expected 12 exact S3 AI avatar state images."
}
Assert-Contains $pixel 'ai_chat_avatar_asset_get\s*\(' `
    "Runtime AI page is not using the exact S3 avatar state assets."
Assert-Contains $pixel 'g_pixel_canvas,\s*8,\s*34,\s*304,\s*24' `
    "Network test summary does not match the S3 320x240 layout."
Assert-Contains $pixel 'TIRTC_DEMO_VIEW_CALL_ADD_EDIT' `
    "Device contact edit flow is missing."
Assert-Contains $pixel 'TIRTC_DEMO_VIEW_WECHAT_ADD_EDIT' `
    "WeChat contact edit flow is missing."

$bindingCallback = [regex]::Match(
    $ui,
    '(?s)static\s+void\s+tirtc_demo_binding_callback\s*\([^)]*\)\s*\{.*?(?=static\s+void\s+tirtc_demo_reset_binding_callback)'
).Value
Assert-Contains $bindingCallback `
    'tirtc_demo_render_view\s*\(\s*TIRTC_DEMO_VIEW_HOME\s*\)' `
    "Binding refresh must stay on the S3 home binding overlay."
Assert-NotContains $bindingCallback `
    'tirtc_demo_render_view\s*\(\s*TIRTC_DEMO_VIEW_CONFIG\s*\)' `
    "Binding refresh still routes to the TiRTC config page."
Assert-Contains $bindingCallback '\[tirtc_ui\].*binding refresh clicked' `
    "Binding refresh click is not logged."

foreach ($pattern in @(
    'overlay,\s*22,\s*32,\s*276,\s*176',
    'dialog,\s*15,\s*33,\s*136,\s*50',
    'dialog,\s*158,\s*30,\s*101,\s*101',
    '"\u7ed1\u5b9a\u7801"',
    '"\u7ed1\u5b9a\u7f51\u5740"',
    '"\u5237\u65b0"',
    '"------"',
    'TIRTC_DEMO_CLOUD_API_BASE'
)) {
    Assert-Contains $pixel $pattern `
        "S3 binding overlay geometry or content is incomplete: $pattern"
}
Assert-NotContains $pixel '"\u7533\u8bf7\u4e2d"|"\u91cd\u65b0\u7533\u8bf7"' `
    "Legacy non-S3 binding copy is still present in the runtime overlay."

foreach ($pattern in @(
    '\[tirtc_cloud\] http dns failed',
    '\[tirtc_cloud\] binding work begin',
    'binding retry scheduled',
    'binding auth_grant without credentials',
    'cloud_apply_bound_identity',
    'next_binding_retry_ms',
    'TIRTC_DEMO_CLOUD_WORK_BIND_POLL',
    'cloud_poll_binding_now',
    'binding refresh existing session',
    'binding HTTP poll received credentials',
    'binding ack puback result=',
    'mqtt connection mode=.*status=.*name=',
    'connected=%d\\n',
    'mqtt request operation=',
    'binding phase=code-issued next=temp-mqtt',
    '(?s)binding phase=temp-mqtt-online.*next=subscribe-cmd',
    'binding phase=auth-grant next=ack-puback',
    '(?s)mqtt tls profile verify=required.*key_exchange=ecdhe-rsa.*curves=secp256r1.*x25519=disabled.*ecdh_context=legacy',
    'TIRTC_DEMO_CLOUD_BINDING_POLL_MS\s+10000U',
    'TIRTC_DEMO_CLOUD_RATE_LIMIT_RETRY_MS\s+30000U'
)) {
    Assert-Contains $cloudSource $pattern `
        "Binding runtime diagnostics or S3 reconcile behavior is missing: $pattern"
}
Assert-Contains $mbedtlsConfig `
    '#define\s+MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED' `
    "MQTT TLS cannot negotiate the broker's required ECDHE-RSA suite."
Assert-Contains $mbedtlsConfig `
    '#define\s+MBEDTLS_ECP_DP_SECP256R1_ENABLED' `
    "The broker-compatible secp256r1 curve is not enabled."
Assert-Contains $mbedtlsConfig `
    '#define\s+MBEDTLS_ECDH_LEGACY_CONTEXT' `
    "The G32-compatible legacy ECDH context is not enabled."
Assert-NotContains $mbedtlsConfig `
    '(?m)^\s*#define\s+MBEDTLS_ECP_DP_CURVE25519_ENABLED' `
    "The broken G32 X25519 path is still offered to the MQTT broker."
Assert-Contains $altcpTls '\[tls\] handshake failed ret=.*reason=' `
    "TLS handshake failures do not expose the mbedTLS error code."
Assert-Contains $altcpTls '\[tls\] handshake ok version=.*cipher=' `
    "Successful TLS handshakes do not report the negotiated cipher."
Assert-Contains $altcpTls '\[tls\] rx stack high-water task=.*free=.*bytes' `
    "The synchronous ATBM RX/TLS path no longer reports its stack margin."
foreach ($pattern in @(
    '\[tls\] ecdh stage=server-params curve=',
    '\[tls\] ecdh stage=read-server-params failed',
    '\[tls\] ecdh stage=make-public (failed|ok)',
    '\[tls\] ecdh stage=calc-secret (failed|ok)'
)) {
    Assert-Contains $sslClient $pattern `
        "ECDH stage diagnostics are missing: $pattern"
}
Assert-Contains $ecdh '(?s)\[tls\] ecdh keygen group=.*n_lsb=.*rng=' `
    "ECDH key generation preconditions are not logged."
Assert-Contains $mbedtlsMakefile `
    '(?s)ifndef\s+CONFIG_WIRELESS_ATBM.*mbedtls_code/library/ecp\.c.*endif' `
    "The active ATBM build no longer selects its private ECP implementation."
Assert-Contains $atbmMakefile 'net/wpa/sae/ecp_extend\.c' `
    "The ATBM build no longer compiles the ECDH private-key implementation."
foreach ($pattern in @(
    '#define\s+MBEDTLS_ECP_SHORT_WEIERSTRASS_ENABLED',
    '(?s)mbedtls_ecp_gen_privkey\s*\(.*?MBEDTLS_ECP_SHORT_WEIERSTRASS_ENABLED.*?ecp_get_type\s*\(\s*grp\s*\).*?ECP_TYPE_SHORT_WEIERSTRASS',
    '(?s)mbedtls_ecp_mul_restartable\s*\(.*?MBEDTLS_ECP_SHORT_WEIERSTRASS_ENABLED.*?ecp_mul_comb\s*\('
)) {
    Assert-Contains $atbmEcpExtend $pattern `
        "The linked ATBM P-256 implementation is still compiled without its short-Weierstrass path: $pattern"
}
foreach ($pattern in @(
    '\[tls\] ecdh keygen stage=gen-private (failed|ok)',
    '\[tls\] ecdh keygen stage=mul-public (failed|ok)'
)) {
    Assert-Contains $ecdh $pattern `
        "The live ECDH key-generation stage diagnostics are missing: $pattern"
}
Assert-Contains $ecdh `
    '(?s)d->p\s*==\s*NULL.*mbedtls_mpi_grow\s*\(\s*d,\s*d_limbs\s*\).*mbedtls_ecp_gen_privkey' `
    "ECDH private-scalar storage is not reserved with the mbedTLS allocator before the ATBM backend writes it."
Assert-Contains $atbmThread '#define\s+ATBM_WIFI_RX_STACK_SIZE\s+\(24U\s*\*\s*1024U\)' `
    "The ATBM RX thread is not provisioned for the synchronous TLS call chain."
Assert-Contains $atbmThread '(?s)strcmp\s*\(\s*name,\s*"atbm_rx"\s*\).*ATBM_WIFI_RX_STACK_SIZE' `
    "The larger stack is not scoped to the ATBM RX thread."
Assert-Contains $lwipOptions '#define\s+TCPIP_THREAD_STACKSIZE\s+8192' `
    "The live TLS trace left only 596 bytes in the 4 KiB tcpip_thread stack."
Assert-NotContains $atbmEcp '\[tls\] atbm ecp keygen group=' `
    "Dead diagnostics remain in mbedtls_ecp_gen_keypair_base; TLS uses mbedtls_ecp_gen_privkey instead."
foreach ($pattern in @(
    '#define\s+TIRTC_VERSION_MAJOR\s+2',
    '#define\s+TIRTC_VERSION_MINOR\s+2',
    '#define\s+TIRTC_VERSION_PATCH\s+1',
    'TIRTC_OPT_CLIENT_ID',
    'TiRtcGetBuildInfo\s*\('
)) {
    Assert-Contains $tirtcHeader $pattern `
        "The G32 TiRTC public contract is not the required v2.2.1 API: $pattern"
}
$tirtcLibraryHash = (Get-FileHash -LiteralPath $tirtcLibraryPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($tirtcLibraryHash -ne '33e889d70d4459587faf57f611e9648d52accb2bc02d0ec75205a52e9ca27fba') {
    throw "Unexpected G32 libTiRTC.a: $tirtcLibraryHash"
}
foreach ($pattern in @(
    'tirtc_demo_cloud_get_physical_client_id\s*\(',
    'TIRTC_OPT_CLIENT_ID\s*,\s*client_id',
    'TiRtcGetBuildInfo\s*\(\s*\)',
    'start returned result=',
    'if\s*\(\s*result\s*!=\s*0\s*\)'
)) {
    Assert-Contains ($appSource + "`n" + $cloudSource) $pattern `
        "The v2.2.1 G32 start contract is incomplete: $pattern"
}
foreach ($pattern in @(
    '#include\s+<driver/rtc\.h>',
    'rtc_get_current_tm\s*\(',
    'rtc_tm_to_time\s*\(&rtc_tm,\s*&local_seconds\)',
    'utc_seconds\s*=\s*\(int64_t\)local_seconds\s*\+\s*\(int64_t\)_timezone',
    'rtc_tm\.tm_year\s*\+\s*1900\s*<\s*2024',
    'systick_get_time_us\s*\(',
    'int\s+_gettimeofday\s*\(',
    'return\s+gettimeofday\s*\(tv,\s*tz_\)'
)) {
    Assert-Contains $gettimeofday $pattern `
        "RTC-backed wall clock or monotonic fallback is missing: $pattern"
}
Assert-Contains $posixClock `
    'clk_id\s*==\s*CLOCK_REALTIME(?s).*gettimeofday\s*\(&tv,\s*NULL\)' `
    "CLOCK_REALTIME is not routed through the RTC-backed wall clock."
Assert-Contains $posixClock 'usecs_to_timespec\s*\(us,\s*tp\)' `
    "Monotonic clock fallback no longer uses the systick source."
Assert-Contains $lvConf '#define\s+LV_FONT_MONTSERRAT_12\s+1' `
    "Montserrat 12 must be enabled for compact ASCII labels."
Assert-Contains $lvConf '#define\s+LV_FONT_MONTSERRAT_20\s+1' `
    "Montserrat 20 must be enabled for the S3 binding code."
Assert-NotContains $pixel `
    'lv_obj_set_style_text_font\s*\(\s*textarea,\s*&jz_ui_font_12' `
    "Compact ASCII inputs still use the incomplete HarmonyOS 12 font."

foreach ($entry in @(
    @{ Name = 'wifi-settings'; Pattern = 'Wi-Fi\s+\u8bbe\u7f6e' },
    @{ Name = 'network-test'; Pattern = '\u7f51\u7edc\u6d4b\u8bd5' },
    @{ Name = 'tirtc-config'; Pattern = 'TiRTC\s+\u914d\u7f6e' },
    @{ Name = 'tirtc-test'; Pattern = 'TiRTC\s+\u6d4b\u8bd5' }
)) {
    Assert-Contains $pixel $entry.Pattern `
        "Missing S3 system entry: $($entry.Name)"
}

Write-Output "G32 S3 runtime UI check: PASS"
Write-Output "Component assets: $assetCount/20"
Write-Output "Fixed CJK copy images: $($fixedCjkStrings.Count)/$($fixedCjkStrings.Count)"
Write-Output "Legacy screenshot shell: absent"
Write-Output "OTA product UI: absent"
Write-Output "Runtime view mappings: $($requiredViews.Count)/$($requiredViews.Count)"
Write-Output "Business API bindings: $($requiredApiBindings.Count)/$($requiredApiBindings.Count)"
Write-Output "S3 AI avatar states: 12/12"
Write-Output "Binding refresh route: home overlay"
Write-Output "Binding runtime diagnostics: MQTT + HTTP poll + PUBACK"
Write-Output "TiRTC SDK: v2.2.1 G32 library + physical CLIENT_ID"
Write-Output "tcpip_thread stack: 8192 bytes"
Write-Output "Wall clock: RTC-backed newlib_riscv syscall; monotonic clock: systick"
Write-Output "Compact ASCII fonts: Montserrat 12/20"
