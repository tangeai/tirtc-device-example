[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_-]+$')]
    [string]$DeviceId,

    [string]$BaseUrl = 'http://api-tirtc.tange365.com',
    [string]$AccessKeyId = $env:TIRTC_ACCESS_KEY_ID,
    [string]$SecretKeyId = $env:TIRTC_SECRET_KEY_ID,
    [string]$AppId = $env:TIRTC_APP_ID,
    [string]$EvidencePath
)

$ErrorActionPreference = 'Stop'
$algorithm = 'TGV1-HMAC-SHA256'
$path = '/v1/devices/unbind_client_id'
$utf8 = [System.Text.UTF8Encoding]::new($false)

function Get-HexSha256 {
    param([byte[]]$Bytes)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Get-HmacSha256 {
    param(
        [byte[]]$Key,
        [string]$Text
    )

    $hmac = [System.Security.Cryptography.HMACSHA256]::new($Key)
    try {
        return $hmac.ComputeHash($utf8.GetBytes($Text))
    }
    finally {
        $hmac.Dispose()
    }
}

foreach ($required in @{
        AccessKeyId = $AccessKeyId
        SecretKeyId = $SecretKeyId
        AppId = $AppId
    }.GetEnumerator()) {
    if ([string]::IsNullOrWhiteSpace([string]$required.Value)) {
        throw "$($required.Key) is required. Pass it explicitly or set the matching TIRTC_* environment variable."
    }
}

$bodyJson = @{ device_ids = @($DeviceId) } | ConvertTo-Json -Compress
$body = $utf8.GetBytes($bodyJson)
$payloadHash = Get-HexSha256 -Bytes $body
$now = [DateTime]::UtcNow
$signingTime = $now.AddTicks(-($now.Ticks % [TimeSpan]::TicksPerSecond))
$tgDate = $signingTime.ToString('yyyyMMddTHHmmssZ', [Globalization.CultureInfo]::InvariantCulture)
$scope = $signingTime.AddDays(7).ToString('yyyyMMdd', [Globalization.CultureInfo]::InvariantCulture) + '/tgv1_request'

$canonicalHeaders = [ordered]@{
    'content-length'      = [string]$body.Length
    'content-type'        = 'application/json'
    'x-tg-algorithm'      = $algorithm
    'x-tg-app-id'         = $AppId.Trim()
    'x-tg-content-sha256' = $payloadHash
    'x-tg-date'           = $tgDate
}
$signedHeaders = ($canonicalHeaders.Keys | Sort-Object) -join ';'
$canonicalHeaderText = (($canonicalHeaders.Keys | Sort-Object | ForEach-Object {
            "${_}:$($canonicalHeaders[$_].Trim())"
        }) -join "`n")
$canonicalRequest = @(
    'POST'
    $path
    ''
    $canonicalHeaderText
    $signedHeaders
    $payloadHash
) -join "`n"
$stringToSign = @(
    $algorithm
    $tgDate
    $scope
    (Get-HexSha256 -Bytes $utf8.GetBytes($canonicalRequest))
) -join "`n"

$key = Get-HmacSha256 -Key $utf8.GetBytes('TGV1' + $SecretKeyId) -Text $signingTime.ToString('yyyyMMdd', [Globalization.CultureInfo]::InvariantCulture)
$key = Get-HmacSha256 -Key $key -Text $path
$key = Get-HmacSha256 -Key $key -Text 'tgv1_request'
$signature = ([BitConverter]::ToString((Get-HmacSha256 -Key $key -Text $stringToSign))).Replace('-', '').ToLowerInvariant()
$authorization = "$algorithm Credential=$AccessKeyId/$scope, SignedHeaders=$signedHeaders, Signature=$signature"

$uri = $BaseUrl.TrimEnd('/') + $path
if (-not $PSCmdlet.ShouldProcess("$uri for device $DeviceId", 'Remove the TiRTC cloud client_id mapping')) {
    return
}

$headers = @{
    'Authorization'       = $authorization
    'X-Tg-Algorithm'      = $algorithm
    'X-Tg-App-Id'         = $AppId.Trim()
    'X-Tg-Content-Sha256' = $payloadHash
    'X-Tg-Date'           = $tgDate
    'X-Tg-Signed-Headers' = $signedHeaders
}

$started = Get-Date
try {
    $response = Invoke-WebRequest -UseBasicParsing -Method Post -Uri $uri -Headers $headers -ContentType 'application/json' -Body $body -TimeoutSec 20
    $statusCode = [int]$response.StatusCode
    $responseBody = [string]$response.Content
}
catch {
    $statusCode = 0
    $responseBody = $_.Exception.Message
    if ($_.Exception.Response) {
        $statusCode = [int]$_.Exception.Response.StatusCode
        try {
            $reader = [System.IO.StreamReader]::new($_.Exception.Response.GetResponseStream())
            $responseBody = $reader.ReadToEnd()
            $reader.Dispose()
        }
        catch {
        }
    }
}

$evidence = [ordered]@{
    timestamp_utc = $started.ToUniversalTime().ToString('o')
    endpoint = $uri
    device_id = $DeviceId
    http_status = $statusCode
    response_body = $responseBody
    elapsed_ms = [int]((Get-Date) - $started).TotalMilliseconds
}

if ($EvidencePath) {
    $parent = Split-Path -Parent $EvidencePath
    if ($parent) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $evidence | ConvertTo-Json | Set-Content -LiteralPath $EvidencePath -Encoding UTF8
}

$evidence
if ($statusCode -ne 200) {
    throw "TiRTC unbind failed: HTTP $statusCode, body=$responseBody"
}
