<#
.SYNOPSIS
    Installs the built conVR driver into SteamVR and enables it.

.DESCRIPTION
    Does the three things that have to happen before SteamVR will load the
    driver, and reports on each one:

      1. Copies build\convr\ into the SteamVR drivers directory. That
         directory normally lives under Program Files (x86), so this script
         re-launches itself elevated when it finds it cannot write there.
      2. Merges "activateMultipleDrivers": true and "driver_convr": { "enable":
         true } into steamvr.vrsettings, preserving everything else in the file
         and taking a timestamped backup first.
      3. Refuses to run while SteamVR is up, because vrserver keeps the old
         driver DLL loaded and locked - the copy would either fail or be
         ignored until the next restart.

    Steam is located from the registry and SteamVR from libraryfolders.vdf, so
    a SteamVR installed to a secondary drive is found without being told.

.PARAMETER BuildDir
    The CMake build directory containing the staged convr\ folder.
    Defaults to "build" beside the repository root.

.PARAMETER DriversDir
    Override the SteamVR drivers directory instead of auto-detecting it.

.PARAMETER SkipSettings
    Copy the driver but leave steamvr.vrsettings alone.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\install-windows.ps1
#>
[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$DriversDir,
    [switch]$SkipSettings,
    # Set when the script re-launches itself elevated; not for direct use.
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build' }

function Write-Step  ($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok    ($m) { Write-Host "    $m" -ForegroundColor Green }
function Write-Warn2 ($m) { Write-Host "    $m" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# Locate Steam, then SteamVR. SteamVR is frequently on a different drive from
# Steam itself, so the library list is the only reliable source.
# ---------------------------------------------------------------------------
function Get-SteamPath {
    foreach ($key in @('HKCU:\Software\Valve\Steam',
                       'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
                       'HKLM:\SOFTWARE\Valve\Steam')) {
        if (-not (Test-Path $key)) { continue }
        $props = Get-ItemProperty -Path $key
        foreach ($name in @('SteamPath', 'InstallPath')) {
            $value = $props.$name
            if ($value -and (Test-Path $value)) { return (Resolve-Path $value).Path }
        }
    }
    $fallback = "${env:ProgramFiles(x86)}\Steam"
    if (Test-Path $fallback) { return $fallback }
    throw "Could not find a Steam installation. Pass -DriversDir explicitly."
}

function Get-SteamLibraries ($steamPath) {
    $libraries = @($steamPath)
    $vdf = Join-Path $steamPath 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        # Every library block carries a "path" line; that is all we need, and
        # matching it directly avoids writing a VDF parser.
        foreach ($match in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libraries += $match.Groups[1].Value -replace '\\\\', '\'
        }
    }
    return $libraries | Select-Object -Unique
}

function Get-SteamVRDriversDir {
    $steamPath = Get-SteamPath
    foreach ($library in (Get-SteamLibraries $steamPath)) {
        $candidate = Join-Path $library 'steamapps\common\SteamVR\drivers'
        if (Test-Path $candidate) { return $candidate }
        # SteamVR present but no drivers dir yet: still the right place.
        $steamvr = Join-Path $library 'steamapps\common\SteamVR'
        if (Test-Path $steamvr) { return $candidate }
    }
    throw "SteamVR is not installed in any Steam library. Install it, or pass -DriversDir."
}

function Get-SteamConfigPath { Join-Path (Get-SteamPath) 'config\steamvr.vrsettings' }

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------
$stagedDriver = Join-Path $BuildDir 'convr\bin\win64\driver_convr.dll'
if (-not (Test-Path $stagedDriver)) {
    throw ("No built driver at $stagedDriver`n" +
           "Build it first:`n" +
           "  cmake -S . -B build -A x64`n" +
           "  cmake --build build --config Release")
}

$running = Get-Process -Name 'vrserver', 'vrmonitor', 'vrcompositor' -ErrorAction SilentlyContinue
if ($running) {
    throw ("SteamVR is running (" + (($running.Name | Select-Object -Unique) -join ', ') +
           "). Close it first - vrserver holds the old driver_convr.dll open, " +
           "so an install now would not take effect.")
}

if (-not $DriversDir) { $DriversDir = Get-SteamVRDriversDir }
$target = Join-Path $DriversDir 'convr'

# ---------------------------------------------------------------------------
# Elevate only if the destination actually needs it.
# ---------------------------------------------------------------------------
function Test-Writable ($path) {
    $probeDir = $path
    while ($probeDir -and -not (Test-Path $probeDir)) { $probeDir = Split-Path -Parent $probeDir }
    if (-not $probeDir) { return $false }
    $probe = Join-Path $probeDir ".convr-write-probe"
    try {
        [System.IO.File]::WriteAllText($probe, 'x')
        Remove-Item $probe -Force
        return $true
    } catch { return $false }
}

if (-not (Test-Writable $DriversDir)) {
    if ($Elevated) { throw "Still cannot write to $DriversDir even elevated." }
    Write-Step "$DriversDir needs administrator rights; re-launching elevated."
    Write-Warn2 "Accept the UAC prompt."

    $argList = @('-ExecutionPolicy', 'Bypass', '-NoProfile', '-File', "`"$PSCommandPath`"",
                 '-BuildDir', "`"$BuildDir`"", '-DriversDir', "`"$DriversDir`"", '-Elevated')
    if ($SkipSettings) { $argList += '-SkipSettings' }

    $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList `
                          -Verb RunAs -Wait -PassThru
    if ($proc.ExitCode -ne 0) { throw "Elevated install failed with exit code $($proc.ExitCode)." }
    Write-Ok "Elevated install finished."
    exit 0
}

# ---------------------------------------------------------------------------
# 1. Copy the staged driver folder
# ---------------------------------------------------------------------------
Write-Step "Installing driver to $target"
if (Test-Path $target) {
    Remove-Item $target -Recurse -Force
}
New-Item -ItemType Directory -Path $target -Force | Out-Null
Copy-Item -Path (Join-Path $BuildDir 'convr\*') -Destination $target -Recurse -Force

foreach ($required in @('driver.vrdrivermanifest',
                        'bin\win64\driver_convr.dll',
                        'resources\input\convr_treadmill_profile.json',
                        'resources\input\legacy_bindings_convr_treadmill.json',
                        # The bindings UI throws and paints itself black if the
                        # profile's binding image is missing, so treat the icon
                        # as load-bearing rather than decoration.
                        'resources\icons\convr_treadmill.svg',
                        'resources\localization\localization.json',
                        'resources\settings\default.vrsettings')) {
    if (-not (Test-Path (Join-Path $target $required))) {
        throw "Install incomplete: $required is missing from $target"
    }
}
Write-Ok "Driver installed."

# ---------------------------------------------------------------------------
# 2. Merge the two settings SteamVR needs
# ---------------------------------------------------------------------------
if (-not $SkipSettings) {
    $settingsPath = Get-SteamConfigPath
    Write-Step "Enabling the driver in $settingsPath"

    $settings = $null
    if (Test-Path $settingsPath) {
        $backup = "$settingsPath.convr-backup-" + (Get-Item $settingsPath).LastWriteTime.ToString('yyyyMMdd-HHmmss')
        Copy-Item $settingsPath $backup -Force
        Write-Ok "Backed up to $(Split-Path -Leaf $backup)"
        $raw = Get-Content $settingsPath -Raw
        if ($raw.Trim()) {
            try { $settings = $raw | ConvertFrom-Json }
            catch { Write-Warn2 "Existing file is not valid JSON; writing a fresh one." }
        }
    } else {
        New-Item -ItemType Directory -Path (Split-Path -Parent $settingsPath) -Force | Out-Null
    }
    if ($null -eq $settings) { $settings = [PSCustomObject]@{} }

    # Merge in place so nothing else the user has configured is lost.
    function Set-JsonValue ($root, $section, $key, $value) {
        if (-not $root.PSObject.Properties.Match($section).Count) {
            $root | Add-Member -NotePropertyName $section -NotePropertyValue ([PSCustomObject]@{})
        }
        $node = $root.$section
        if ($node.PSObject.Properties.Match($key).Count) { $node.$key = $value }
        else { $node | Add-Member -NotePropertyName $key -NotePropertyValue $value }
    }

    # Without activateMultipleDrivers, SteamVR loads only the driver that
    # provides the headset and conVR never gets a look in.
    Set-JsonValue $settings 'steamvr'      'activateMultipleDrivers' $true
    Set-JsonValue $settings 'driver_convr' 'enable'                  $true

    ($settings | ConvertTo-Json -Depth 20) | Set-Content -Path $settingsPath -Encoding utf8
    Write-Ok 'steamvr.activateMultipleDrivers = true'
    Write-Ok 'driver_convr.enable = true'
}

Write-Host ''
Write-Step 'Done.'
Write-Host '    Start SteamVR, then run build\bin\convr_companion.exe.'
Write-Host '    The companion should report the driver link as connected within a second.'
