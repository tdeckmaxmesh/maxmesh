# Shared paths for the maxmesh build scripts (dot-sourced).
# Finds Git, Python (with esptool) and PlatformIO on this PC.

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # MeshCore repository root

if (Test-Path "C:\Program Files\Git\cmd") { $env:PATH = "C:\Program Files\Git\cmd;$env:PATH" }   # pio needs git

$Py = $null
foreach ($c in @("python", "py",
                 "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe",
                 "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe")) {
  try { & $c --version *> $null; if ($LASTEXITCODE -eq 0) { $Py = $c; break } } catch { }
}
if (-not $Py) { throw "Python 3 not found - install it from https://www.python.org" }

$Pio = (Get-Command pio -ErrorAction SilentlyContinue).Source
if (-not $Pio) {
  $cand = Get-ChildItem "$env:APPDATA\Python\Python3*\Scripts\pio.exe", "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($cand) { $Pio = $cand.FullName }
}
if (-not $Pio) { throw "PlatformIO not found - install it with:  $Py -m pip install --user platformio" }

$BootApp0 = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"
