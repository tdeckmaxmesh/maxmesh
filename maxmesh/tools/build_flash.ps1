# Build maxmesh for the T-Deck MAX and flash it over USB.
#   .\build_flash.ps1            build + flash app (keeps contacts/messages)
#   .\build_flash.ps1 -Full      build + flash bootloader/partitions/app (fresh install layout)
#   .\build_flash.ps1 -Erase     wipe the whole chip first (factory reset), then full flash
#   .\build_flash.ps1 -NoFlash   just build
param([string]$Port = "COM4", [switch]$Full, [switch]$Erase, [switch]$NoFlash)

$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "env.ps1")
$envName = "LilyGo_TDeck_Max_standalone_ble"

Push-Location $RepoRoot
try {
  & $Pio run -e $envName
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
  if ($NoFlash) { return }
  $b = ".pio\build\$envName"
  if ($Erase) { & $Py -m esptool --chip esp32s3 --port $Port erase-flash; Start-Sleep 2; $Full = $true }
  if ($Full) {
    & $Py -m esptool --chip esp32s3 --port $Port --baud 921600 write-flash -z --flash-mode dio --flash-freq 80m --flash-size 16MB `
      0x0 "$b\bootloader.bin" 0x8000 "$b\partitions.bin" 0xe000 $BootApp0 0x10000 "$b\firmware.bin"
  } else {
    & $Py -m esptool --chip esp32s3 --port $Port --baud 921600 write-flash -z 0x10000 "$b\firmware.bin"
  }
  if ($LASTEXITCODE -ne 0) { throw "flash failed" }
} finally { Pop-Location }
