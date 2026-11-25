# maxmesh v{VER} installer for the LilyGo T-Deck MAX
#
#   Right-click this file -> "Run with PowerShell"   (fresh install: ERASES the device)
#   or in PowerShell:  .\flash-maxmesh.ps1 -Update   (upgrade, keeps contacts/messages)
#   Optional: -Port COM5 to choose the port yourself.
param([string]$Port = "", [switch]$Update)
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Pause-Exit($msg) { Write-Host ""; Write-Host $msg; Read-Host "Press Enter to close" | Out-Null; exit }

# ---- find Python + esptool ----
$py = $null
foreach ($c in @("$env:LOCALAPPDATA\Programs\Python\Python312\python.exe", "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe", "py", "python")) {
  try { & $c --version *> $null; if ($LASTEXITCODE -eq 0) { $py = $c; break } } catch { }
}
if (-not $py) { Pause-Exit "Python 3 is not installed. Install it from https://www.python.org (or use the browser method in HOW-TO-INSTALL.txt)." }
& $py -m esptool version *> $null
if ($LASTEXITCODE -ne 0) {
  Write-Host "Installing esptool (one time)..."
  & $py -m pip install --user esptool
}

# ---- find the T-Deck (Espressif USB, VID 303A) ----
if (-not $Port) {
  $dev = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match "VID_303A" } | Select-Object -First 1
  if ($dev -and $dev.FriendlyName -match "\((COM\d+)\)") { $Port = $Matches[1] }
}
if (-not $Port) {
  Pause-Exit ("T-Deck MAX not found on USB.`n" +
    "  1. Plug it in with a data USB-C cable.`n" +
    "  2. If it still isn't found: hold the side rocker UP (BOOT), tap the rocker DOWN (reset), release BOOT, then run this again.")
}
Write-Host "T-Deck MAX found on $Port"

if ($Update) {
  & $py -m esptool --chip esp32s3 --port $Port --baud 921600 write-flash -z 0x10000 "$here\maxmesh-v{VER}-update.bin"
} else {
  Write-Host ""
  Write-Host "FRESH INSTALL: this ERASES everything on the device (contacts, channels, messages, name, keys)."
  $ok = Read-Host "Type YES to continue"
  if ($ok -ne "YES") { Pause-Exit "Cancelled - nothing was changed." }
  & $py -m esptool --chip esp32s3 --port $Port erase-flash
  if ($LASTEXITCODE -ne 0) { Pause-Exit "Erase failed. Try the BOOT + reset step above, then run again." }
  Start-Sleep 2
  & $py -m esptool --chip esp32s3 --port $Port --baud 921600 write-flash -z 0x0 "$here\maxmesh-v{VER}-factory.bin"
}
if ($LASTEXITCODE -ne 0) { Pause-Exit "Flashing failed. Try the BOOT + reset step above, then run again." }
Pause-Exit "Done! The T-Deck restarts into maxmesh v{VER}. The first start takes ~1 minute while it prepares storage."
