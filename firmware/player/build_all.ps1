<#
.SYNOPSIS
  Builds the player firmware (ESP32-S3, ESP-IDF + ESP-ADF) and writes the release files to .\release.

.DESCRIPTION
  Uses the ESP-IDF installation selected by -Activation (the EIM activation script of the IDF the project
  is configured for, v5.5.4).  The existing build dir (.\build) is reused, so this is incremental.

  Output (release\):
    musicplayer.bin       application image: upload it on the player's own update page (OTA)

.PARAMETER Clean
  Run "idf.py fullclean" first.
.PARAMETER Activation
  EIM activation script of the ESP-IDF to use.
#>
param(
    [switch]$Clean,
    [string]$Activation = 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path $Activation)) { throw "ESP-IDF activation script not found: $Activation (use -Activation)" }
    # The activation script prints progress on stderr; don't let that abort this script.
    $ErrorActionPreference = 'Continue'
    . $Activation *> $null
    $ErrorActionPreference = 'Stop'
    if (-not $env:IDF_PATH) { throw 'Could not activate ESP-IDF (run this from PowerShell 7 / pwsh, not Git Bash)' }
}

$releaseDir = Join-Path $PSScriptRoot 'release'
New-Item -ItemType Directory -Force $releaseDir | Out-Null

Write-Host "`n=== player (esp32s3) ===" -ForegroundColor Cyan
if ($Clean) { idf.py fullclean; if ($LASTEXITCODE -ne 0) { throw 'idf.py fullclean failed' } }

idf.py build *>&1 | ForEach-Object {
    if ($_ -match 'binary size|Project build complete| error:|CMake Error|FAILED') { Write-Host $_ }
}
if ($LASTEXITCODE -ne 0) { throw 'player build failed' }

$build = Join-Path $PSScriptRoot 'build'
$app = Join-Path $build 'musicplayer.bin'
if (-not (Test-Path $app)) { throw "musicplayer.bin not found in $build" }

Copy-Item $app (Join-Path $releaseDir 'musicplayer.bin') -Force

Write-Host "`n=== Player release files ===" -ForegroundColor Cyan
Get-ChildItem $releaseDir -Filter '*.bin' | ForEach-Object {
    Write-Host ("{0,-24} {1,12:N0} bytes" -f $_.Name, $_.Length)
}
