<#
.SYNOPSIS
  Builds everything (player + both display variants) and collects the release binaries in .\release,
  with a yyyymmdd date stamp in every file name.

.DESCRIPTION
  firmware\display\build_all.ps1  -> ESP32-8048S050C-full.bin, ESP32-2432S032C-full.bin   (ESP-IDF 5.5.2)
  firmware\player\build_all.ps1   -> musicplayer.bin (OTA)                                (ESP-IDF 5.5.4 + ADF)

  Both run in their own PowerShell process because they use different ESP-IDF installations.

  release\ESP32-8048S050C-full-<date>.bin    display, 800x480 (ESP32-S3)   - flash with the player update page (address: auto)
  release\ESP32-2432S032C-full-<date>.bin    display, 320x240 (ESP32)      - flash with the player update page (address: auto)
  release\musicplayer-<date>.bin             player application image      - upload on the player update page (OTA)

.PARAMETER Clean
  Clean build of everything (passed on to the firmware scripts).
.PARAMETER SkipPlayer
  Do not build the player firmware.
.PARAMETER SkipDisplay
  Do not build the display firmware.
.PARAMETER Date
  Date stamp, default today (yyyymmdd).
.PARAMETER PlayerActivation
  EIM activation script of the ESP-IDF used for the player (default: v5.5.4).

.EXAMPLE
  .\build_all.ps1
  .\build_all.ps1 -SkipPlayer
#>
param(
    [switch]$Clean,
    [switch]$SkipPlayer,
    [switch]$SkipDisplay,
    [string]$Date = (Get-Date -Format 'yyyyMMdd'),
    [string]$PlayerActivation = 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if ($Date -notmatch '^\d{8}$') { throw "-Date must be yyyymmdd (got '$Date')" }
if ($SkipPlayer -and $SkipDisplay) { throw 'Nothing to build (-SkipPlayer and -SkipDisplay)' }
# Run the firmware scripts with the same kind of PowerShell that runs this one (pwsh 7 or Windows PowerShell).
$psExe = (Get-Process -Id $PID).Path
$releaseDir = Join-Path $PSScriptRoot 'release'

function Invoke-Child([string]$Title, [string]$Script, [string[]]$ScriptArgs) {
    Write-Host "`n################ $Title ################" -ForegroundColor Yellow
    & $psExe -NoProfile -ExecutionPolicy Bypass -File $Script @ScriptArgs
    if ($LASTEXITCODE -ne 0) { throw "$Title failed (exit code $LASTEXITCODE)" }
}

# Files to collect: source path -> release base name (the date stamp is inserted before .bin)
$collect = @()
$sw = [Diagnostics.Stopwatch]::StartNew()

if (-not $SkipDisplay) {
    $childArgs = @(); if ($Clean) { $childArgs += '-Clean' }
    Invoke-Child 'Display firmware' (Join-Path $PSScriptRoot 'firmware\display\build_all.ps1') $childArgs
    foreach ($n in 'ESP32-8048S050C-full', 'ESP32-2432S032C-full') {
        $collect += [pscustomobject]@{ Src = (Join-Path $PSScriptRoot "firmware\display\release\$n.bin"); Name = $n }
    }
}
if (-not $SkipPlayer) {
    $childArgs = @('-Activation', $PlayerActivation); if ($Clean) { $childArgs += '-Clean' }
    Invoke-Child 'Player firmware' (Join-Path $PSScriptRoot 'firmware\player\build_all.ps1') $childArgs
    $collect += [pscustomobject]@{ Src = (Join-Path $PSScriptRoot 'firmware\player\release\musicplayer.bin'); Name = 'musicplayer' }
}

# Nothing is copied unless every expected file exists (no half-finished releases)
foreach ($c in $collect) { if (-not (Test-Path $c.Src)) { throw "Expected build output is missing: $($c.Src)" } }

New-Item -ItemType Directory -Force $releaseDir | Out-Null
$released = @()
foreach ($c in $collect) {
    $dst = Join-Path $releaseDir ("{0}-{1}.bin" -f $c.Name, $Date)
    Copy-Item $c.Src $dst -Force
    $released += Get-Item $dst
}

Write-Host "`n=== Release ($Date) in $releaseDir ===" -ForegroundColor Cyan
foreach ($f in $released) { Write-Host ("{0,-46} {1,12:N0} bytes" -f $f.Name, $f.Length) }
Write-Host ("Done in {0:mm\:ss}." -f $sw.Elapsed)
