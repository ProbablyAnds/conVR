<#
.SYNOPSIS
    Checks every link in the conVR chain and says which one is broken.

.DESCRIPTION
    Walks the chain in order - device, driver install, settings, SteamVR,
    driver load, IPC endpoint, companion - and reports each link pass/fail.
    The first FAIL is the one to fix; everything downstream of a broken link
    fails as a consequence, not as a separate problem.

    Safe to run at any time. It only reads.

.PARAMETER Follow
    Keep re-checking every 2 seconds instead of reporting once. Useful while
    starting SteamVR, so you see each link come up.
#>
[CmdletBinding()]
param([switch]$Follow)

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot 'build'

function Get-SteamPath {
    foreach ($key in @('HKCU:\Software\Valve\Steam',
                       'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')) {
        if (-not (Test-Path $key)) { continue }
        $props = Get-ItemProperty -Path $key
        foreach ($name in @('SteamPath', 'InstallPath')) {
            if ($props.$name -and (Test-Path $props.$name)) { return $props.$name }
        }
    }
    return "${env:ProgramFiles(x86)}\Steam"
}

function Show ($ok, $label, $detail) {
    $mark = if ($ok -eq $true) { '[ OK ]' } elseif ($ok -eq $null) { '[ -- ]' } else { '[FAIL]' }
    $colour = if ($ok -eq $true) { 'Green' } elseif ($ok -eq $null) { 'DarkGray' } else { 'Red' }
    Write-Host $mark -ForegroundColor $colour -NoNewline
    Write-Host " $label" -NoNewline
    if ($detail) { Write-Host "  $detail" -ForegroundColor DarkGray } else { Write-Host '' }
}

function Invoke-Check {
    $steam = Get-SteamPath
    $log = Join-Path $steam 'logs\vrserver.txt'

    Clear-Host
    Write-Host "conVR status  -  $(Get-Date -Format 'HH:mm:ss')" -ForegroundColor Cyan
    Write-Host ('-' * 62)

    # 1. Treadmill present as a USB HID gamepad.
    $hid = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
           Where-Object { $_.InstanceId -match 'VID_2E8A' -and $_.Class -eq 'HIDClass' -and
                          $_.FriendlyName -match 'game controller' }
    Show ([bool]$hid) 'Treadmill connected' $(if ($hid) { $hid[0].FriendlyName } else { 'no VID_2E8A game controller found' })

    # 2. Driver installed, and matching what was last built.
    $installed = Join-Path $steam 'steamapps\common\SteamVR\drivers\convr\bin\win64\driver_convr.dll'
    $built = Join-Path $BuildDir 'convr\bin\win64\driver_convr.dll'
    $isInstalled = Test-Path $installed
    Show $isInstalled 'Driver installed' $installed
    if ($isInstalled -and (Test-Path $built)) {
        $same = (Get-FileHash $installed).Hash -eq (Get-FileHash $built).Hash
        Show $same 'Installed driver is current' $(if ($same) { 'matches build' } else { 'STALE - re-run scripts\install-windows.ps1' })
    }

    # 3. The two settings SteamVR needs.
    $cfg = Join-Path $steam 'config\steamvr.vrsettings'
    if (Test-Path $cfg) {
        $s = Get-Content $cfg -Raw | ConvertFrom-Json
        Show ($s.steamvr.activateMultipleDrivers -eq $true) 'activateMultipleDrivers' 'lets conVR load alongside your HMD driver'
        Show ($s.driver_convr.enable -eq $true) 'driver_convr enabled' $null
    } else {
        Show $false 'steamvr.vrsettings' 'not found'
    }

    # 4. SteamVR itself.
    $vr = Get-Process vrserver -ErrorAction SilentlyContinue
    Show ([bool]$vr) 'SteamVR (vrserver) running' $(if (-not $vr) { 'start SteamVR' })

    # 5. Did vrserver actually load the driver? Only meaningful once it is up.
    if ($vr -and (Test-Path $log)) {
        $lines = Get-Content $log -ErrorAction SilentlyContinue
        $loaded = $lines | Select-String -SimpleMatch 'Loaded server driver convr' | Select-Object -Last 1
        $active = $lines | Select-String -SimpleMatch 'treadmill device activated' | Select-Object -Last 1
        $conn   = $lines | Select-String -SimpleMatch 'companion connected' | Select-Object -Last 1
        $rx     = $lines | Select-String -SimpleMatch 'rx packets=' | Select-Object -Last 1
        Show ([bool]$loaded) 'Driver loaded by vrserver' $(if (-not $loaded) { 'no "Loaded server driver convr" in the log yet' })
        Show ([bool]$active) 'Treadmill device activated' $null
        Show ([bool]$conn)   'Driver saw the companion' $null
        if ($rx) { Show $true 'Driver receiving input' ($rx.Line.Trim()) }
        else { Show $null 'Driver receiving input' 'needs CONVR_DEBUG=1 set before Steam started' }
    } else {
        Show $null 'Driver loaded by vrserver' 'waiting for SteamVR'
    }

    # 6. The IPC endpoint, which only exists while the driver is live.
    $pipe = $false
    try { $pipe = [System.IO.Directory]::GetFiles('\\.\pipe\') -match 'convr_treadmill' } catch { }
    Show ([bool]$pipe) 'IPC pipe open' '\\.\pipe\convr_treadmill'

    # 7. The companion.
    $comp = Get-Process convr_companion -ErrorAction SilentlyContinue
    Show ([bool]$comp) 'Companion running' $(if (-not $comp) { "start $BuildDir\bin\convr_companion.exe" })

    Write-Host ('-' * 62)
    Write-Host 'First FAIL is the one to fix; the rest follow from it.' -ForegroundColor DarkGray
}

if ($Follow) {
    Write-Host 'Following - Ctrl+C to stop.' -ForegroundColor Yellow
    while ($true) { Invoke-Check; Start-Sleep -Seconds 2 }
} else {
    Invoke-Check
}
