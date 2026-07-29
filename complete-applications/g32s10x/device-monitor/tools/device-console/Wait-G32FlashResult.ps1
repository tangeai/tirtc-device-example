param(
    [string]$SessionFile,
    [ValidateRange(5, 900)]
    [int]$TimeoutSeconds = 300
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $SessionFile) {
    $SessionFile = Join-Path $repo 'building\cloner_session_latest.json'
}
if (-not (Test-Path -LiteralPath $SessionFile)) {
    throw "Missing Cloner session file: $SessionFile"
}

$session = Get-Content -LiteralPath $SessionFile -Raw -Encoding UTF8 | ConvertFrom-Json
$baseline = @{}
foreach ($entry in @($session.logs)) {
    $baseline[$entry.path] = [long]$entry.length
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$matchedFile = $null
$matchedText = $null
do {
    foreach ($file in @(Get-ChildItem -LiteralPath $session.log_root -File -Filter '*.log' -ErrorAction SilentlyContinue)) {
        $offset = if ($baseline.ContainsKey($file.FullName)) { $baseline[$file.FullName] } else { 0L }
        if ($file.Length -lt $offset) { $offset = 0L }
        if ($file.Length -eq $offset) { continue }

        $stream = [System.IO.File]::Open($file.FullName, 'Open', 'Read', 'ReadWrite')
        try {
            if ($stream.Length -lt $offset) { $offset = 0L }
            [void]$stream.Seek($offset, [System.IO.SeekOrigin]::Begin)
            $remaining = [int]($stream.Length - $offset)
            $bytes = [byte[]]::new($remaining)
            $read = $stream.Read($bytes, 0, $remaining)
            $text = [System.Text.Encoding]::Default.GetString($bytes, 0, $read)
        } finally {
            $stream.Dispose()
        }

        if ($text -match '(?im)write ret:\s*(?:fail|error)|BOOT ERROR|write failed|FAILED') {
            throw "Cloner reported a failure in $($file.FullName):`n$text"
        }
        if ($text -match '(?m)^all policy completed\s*$') {
            $expectedPolicies = [int]$session.expected_policy_count
            for ($index = 0; $index -lt $expectedPolicies; $index++) {
                if ($text -notmatch "(?m)^policy$index write ret:\s*ok\s*$") {
                    throw "Cloner completion is missing policy$index success in $($file.FullName)."
                }
            }
            $matchedFile = $file.FullName
            $matchedText = $text.Trim()
            break
        }
    }
    if ($matchedFile) { break }
    Start-Sleep -Milliseconds 500
} while ((Get-Date) -lt $deadline)

if (-not $matchedFile) {
    throw "Cloner did not append 'all policy completed' within $TimeoutSeconds seconds. Session: $SessionFile"
}

$resultDir = Split-Path -Parent ([System.IO.Path]::GetFullPath($SessionFile))
$resultPath = Join-Path $resultDir ("cloner_result_{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
[System.IO.File]::WriteAllText($resultPath, $matchedText, [System.Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    Completed = $true
    Evidence = 'all policy completed'
    ClonerLog = $matchedFile
    ResultLog = $resultPath
    Session = $SessionFile
}
