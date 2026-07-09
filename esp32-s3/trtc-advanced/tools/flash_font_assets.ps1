param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [int]$Baud = 921600,
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $fontImages = @(@('build', 'build_verify') |
        ForEach-Object {
            $candidate = Join-Path $projectRoot $_
            $image = Join-Path $candidate 'fonts.bin'
            if (Test-Path $image) {
                Get-Item $image
            }
        })

    if ($fontImages.Count -eq 0) {
        throw 'Font image not found in build or build_verify. Run idf.py build first or pass -BuildDir.'
    }

    $buildRoot = Split-Path -Parent (($fontImages | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName)
} elseif ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $buildRoot = $BuildDir
} else {
    $buildRoot = Join-Path $projectRoot $BuildDir
}

Write-Host "Using build directory: $buildRoot"

$fontImage = Join-Path $buildRoot 'fonts.bin'
$partitionTable = Join-Path $buildRoot 'partition_table/partition-table.bin'
$idfPath = $env:IDF_PATH

if ([string]::IsNullOrWhiteSpace($idfPath)) {
    $defaultIdf = 'C:/esp/v5.5.4/esp-idf'
    if (Test-Path $defaultIdf) {
        $idfPath = $defaultIdf
    }
}

if ([string]::IsNullOrWhiteSpace($idfPath) -or !(Test-Path $idfPath)) {
    throw 'IDF_PATH is not set and C:/esp/v5.5.4/esp-idf was not found.'
}
if (!(Test-Path $fontImage)) {
    throw "Font image not found: $fontImage. Run idf.py build first."
}
if (!(Test-Path $partitionTable)) {
    throw "Partition table not found: $partitionTable. Run idf.py build first."
}

$parttool = Join-Path $idfPath 'components/partition_table/parttool.py'
python $parttool `
    --port $Port `
    --baud $Baud `
    --partition-table-file $partitionTable `
    write_partition `
    --partition-name fonts `
    --input $fontImage

if ($LASTEXITCODE -ne 0) {
    throw "Font partition flash failed with exit code $LASTEXITCODE"
}

Write-Host "Font partition flashed: $fontImage -> fonts"
