<#
.SYNOPSIS
  Builds both display firmware variants, each in its own build dir and sdkconfig.

.DESCRIPTION
  8048S050C : ESP32-S3, 800x480   -> build_8048s050c / sdkconfig.8048s050c
  2432S032  : ESP32,    320x240   -> build_2432s032  / sdkconfig.2432s032
  For each variant the result to flash is release\<project>-full.bin (bootloader + partition table + app),
  written by the player update page with flash address "auto".

.PARAMETER Board
  Which variant(s) to build: all (default), 8048s050c or 2432s032.
.PARAMETER Clean
  Remove the variant's build dir and sdkconfig first (forces set-target).
.PARAMETER IdfPath
  ESP-IDF checkout (default: $env:IDF_PATH, else ~/esp/v5.5.2/esp-idf).
.PARAMETER ToolsPath
  IDF_TOOLS_PATH (default: $env:IDF_TOOLS_PATH, else C:\Espressif\tools).

.EXAMPLE
  .\build_all.ps1
  .\build_all.ps1 -Board 2432s032 -Clean
#>
param(
    [ValidateSet('all', '8048s050c', '2432s032')][string]$Board = 'all',
    [switch]$Clean,
    [string]$IdfPath,
    [string]$ToolsPath
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not $IdfPath)   { $IdfPath = if ($env:IDF_PATH) { $env:IDF_PATH } else { Join-Path $HOME 'esp\v5.5.2\esp-idf' } }
if (-not $ToolsPath) { $ToolsPath = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { 'C:\Espressif\tools' } }
if (-not (Test-Path (Join-Path $IdfPath 'export.ps1'))) { throw "ESP-IDF not found at '$IdfPath' (use -IdfPath)" }

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    $env:IDF_TOOLS_PATH = $ToolsPath
    # export.ps1 prints progress on stderr; don't let that abort the script
    $ErrorActionPreference = 'Continue'
    $null = & (Join-Path $IdfPath 'export.ps1') 2>&1   # call (not dot-source): export.ps1 derives IDF_PATH from $PSScriptRoot
    $ErrorActionPreference = 'Stop'
    if (-not $env:IDF_PATH) { throw 'Could not activate ESP-IDF (use PowerShell 7 / pwsh, or run from an IDF shell)' }
}

$releaseDir = Join-Path $PSScriptRoot 'release'
New-Item -ItemType Directory -Force $releaseDir | Out-Null

$variants = @(
    @{ Name = '8048s050c'; Target = 'esp32s3'; Defaults = 'sdkconfig.defaults' },
    @{ Name = '2432s032';  Target = 'esp32';   Defaults = 'sdkconfig.defaults.2432s032' }
)

$results = @()
foreach ($v in $variants) {
    if ($Board -ne 'all' -and $Board -ne $v.Name) { continue }

    $buildDir  = "build_$($v.Name)"
    $sdkconfig = "sdkconfig.$($v.Name)"
    Write-Host "`n=== $($v.Name) ($($v.Target)) ===" -ForegroundColor Cyan

    if ($Clean) { Remove-Item -Recurse -Force $buildDir, $sdkconfig -ErrorAction SilentlyContinue }

    # IDF_TARGET steers the first configure; set-target is not needed (and fails on a fresh dir).
    $env:IDF_TARGET = $v.Target
    $log = Join-Path $buildDir 'build_all.log'
    New-Item -ItemType Directory -Force $buildDir | Out-Null

    idf.py -B $buildDir -D "SDKCONFIG=$sdkconfig" -D "SDKCONFIG_DEFAULTS=$($v.Defaults)" build *>&1 |
        Tee-Object -FilePath $log | ForEach-Object {
            if ($_ -match 'binary size|Project build complete| error:|CMake Error|FAILED') { Write-Host $_ }
        }
    $ok = ($LASTEXITCODE -eq 0)
    $bin = Get-ChildItem $buildDir -Filter '*.bin' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notmatch 'bootloader|partition|ota_data|-full' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    # The deliverable is the merged full image (bootloader + partition table + app). The player update page
    # flashes it with address "auto" (0x1000 on ESP32, 0x0 on ESP32-S3, chosen from the detected chip).
    # The merged image is written to release\ (only -full.bin files live there); the app-only .bin that
    # ESP-IDF creates stays inside the build dir as an intermediate (ESP-IDF's size check needs it).
    $full = $null
    if ($ok -and $bin) {
        $fa = Get-Content (Join-Path $buildDir 'flasher_args.json') -Raw | ConvertFrom-Json
        $offsets = $fa.flash_files.PSObject.Properties | Sort-Object { [Convert]::ToInt32($_.Name, 16) }
        $minOff  = $offsets[0].Name
        $full    = Join-Path $releaseDir ($bin.BaseName + '-full.bin')
        $mergeArgs = @('-m', 'esptool', '--chip', $v.Target, 'merge_bin', '-o', $full, '--target-offset', $minOff,
                       '--flash_mode', $fa.flash_settings.flash_mode, '--flash_freq', $fa.flash_settings.flash_freq,
                       '--flash_size', $fa.flash_settings.flash_size)
        foreach ($o in $offsets) { $mergeArgs += @($o.Name, (Join-Path $buildDir $o.Value)) }
        & python @mergeArgs *>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Host "merge_bin failed for $($v.Name)" -ForegroundColor Red; $ok = $false; $full = $null }
        else { Write-Host ("Full image: {0} (flash at {1})" -f $full, $minOff) }
    }
    $results += [pscustomobject]@{ Variant = $v.Name; Target = $v.Target; OK = $ok; Binary = $bin.FullName; FullImage = $full }
}
$env:IDF_TARGET = $null

Write-Host "`n=== Firmware to flash (player update page, address: auto) ===" -ForegroundColor Cyan
foreach ($r in $results) {
    $state = if ($r.OK) { 'OK    ' } else { 'FAILED' }
    $size  = if ($r.FullImage -and (Test-Path $r.FullImage)) { '{0:N0} bytes' -f (Get-Item $r.FullImage).Length } else { '-' }
    Write-Host ("{0} {1,-10} {2,-8} {3} ({4})" -f $state, $r.Variant, $r.Target, $r.FullImage, $size)
}
if ($results | Where-Object { -not $_.OK }) { exit 1 }
