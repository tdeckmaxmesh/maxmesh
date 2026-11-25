# Build a maxmesh release for the LilyGo T-Deck MAX.
#   .\make_release.ps1            -> maxmesh\release\maxmesh-v<version>\
# Produces:
#   maxmesh-v<ver>-factory.bin   bootloader + partitions + app, flash at 0x0 AFTER a full erase
#   maxmesh-v<ver>-update.bin    app only, flash at 0x10000 (keeps contacts/messages)
#   flash-maxmesh.ps1 / HOW-TO-INSTALL.txt (from tools\release_files)
# Attach these files to a GitHub Release; the release folder itself is git-ignored.
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "env.ps1")
$envName = "LilyGo_TDeck_Max_standalone_ble"

# version from the variant's platformio.ini (-D MAXMESH_VERSION='"1.0"')
$ini = Get-Content (Join-Path $RepoRoot "variants\lilygo_tdeck_max\platformio.ini") -Raw
if ($ini -notmatch "MAXMESH_VERSION='`"([^`"]+)`"'") { throw "MAXMESH_VERSION not found" }
$ver = $Matches[1]
$out = Join-Path $RepoRoot "maxmesh\release\maxmesh-v$ver"
New-Item -ItemType Directory -Force $out | Out-Null

Push-Location $RepoRoot
try {
  & $Pio run -e $envName
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
  $b = ".pio\build\$envName"
  & $Py -m esptool --chip esp32s3 merge-bin -o "$out\maxmesh-v$ver-factory.bin" `
      --flash-mode dio --flash-freq 80m --flash-size 16MB `
      0x0 "$b\bootloader.bin" 0x8000 "$b\partitions.bin" 0xe000 $BootApp0 0x10000 "$b\firmware.bin"
  if ($LASTEXITCODE -ne 0) { throw "merge failed" }
  Copy-Item "$b\firmware.bin" "$out\maxmesh-v$ver-update.bin" -Force
} finally { Pop-Location }

foreach ($f in Get-ChildItem (Join-Path $PSScriptRoot "release_files")) {
  (Get-Content $f.FullName -Raw).Replace("{VER}", $ver) | Set-Content -Encoding ascii (Join-Path $out $f.Name)
}
Get-ChildItem $out | Format-Table Name, Length -AutoSize
