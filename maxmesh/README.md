# maxmesh — LilyGo T-Deck MAX firmware (MeshCore with a full on-device UI)

**maxmesh** turns the LilyGo **T-Deck MAX** (ESP32-S3, SX1262 LoRa, 3.1" e-paper, QWERTY keyboard)
into a complete standalone [MeshCore](https://github.com/meshcore-dev/MeshCore) messenger, like the
regular T-Deck firmware, while still letting the MeshCore phone app pair over Bluetooth at the same time.

It is built directly on upstream MeshCore (v1.18.0 companion-radio core) with a new T-Deck MAX board
port and a keyboard/e-paper UI. The changes to upstream code are small and additive, so newer MeshCore
releases can be merged in.

> Status: beta. Tested on one T-Deck MAX on a live US mesh.

## Install

Download the files from the **Releases** page, then either:

- **Browser (no software):** open <https://espressif.github.io/esptool-js/> in Chrome/Edge → Connect →
  *Erase Flash* → flash `maxmesh-v<ver>-factory.bin` at address `0x0` → press reset.
- **Windows:** run `flash-maxmesh.ps1` from the release (needs Python 3).

`HOW-TO-INSTALL.txt` in each release has the full steps. A fresh install starts like a new device:
it asks for your name and creates a new identity. To upgrade without losing data, flash
`maxmesh-v<ver>-update.bin` at `0x10000` instead.

If the device doesn't show up on USB: hold the side rocker **up** (BOOT), tap it **down** (reset), release.

## Features

- **Messages** inbox for DMs, channels and room servers: unread counts, "New Messages" divider,
  delivery status with automatic retry then flood, fast jump to newest/oldest
- **Channels**: `#hashtag`, private (with key) and Public; show/share keys
- **Contacts**: filters (people/repeaters/rooms/favourites), search, favourites, info with distance
  and bearing, reset path, share, delete; auto-add with *replace oldest non-favourite* (up to 500)
- **Repeater & room admin**: admin/guest login, live status (battery, uptime, noise, RSSI/SNR, packet
  counters, airtime), advert, clock sync, neighbours, version, radio, custom CLI commands, reboot
- **Spell check** in the message box (dashed underline, personal dictionary)
- **Heard nearby**, zero-hop and flood adverts, **GPS** with location sharing
- **Settings**: name, radio presets or custom freq/BW/SF/CR, TX power, antenna, Bluetooth + fixed PIN,
  vibration, frontlight, keyboard light, auto-lock, timezone, clock, e-ink refresh
- **Lock screen** with unread summary; message history saved across reboots (crash-safe writes)
- **Battery**: 80 MHz CPU, unused hardware powered off, light sleep between events when Bluetooth is off

## Controls

The keyboard has no arrow keys, so navigation uses letters, Alt and the touch keys under the screen.

| Where | Keys |
|---|---|
| Lists / menus | **W/S** up/down, **A/D** page, **Enter** open, **Q** or **Backspace** back, shown letters = shortcuts |
| Touch keys | **heart** = up, **plane** = down; **hold** = jump to oldest / newest (first / last item) |
| Typing | **Enter** send/OK, **Backspace** delete, **Alt+Q** cancel / back |
| Chat | **Alt+W/S** scroll, **Enter on empty box** = newest, **Alt+T / Alt+E** oldest / newest, **Alt+M** contact menu, **Alt+K** add word to dictionary |
| Symbols | **Sym** = one symbol (hold or double-tap = lock), **Shift** = capital (double-tap = caps lock) |
| Lights | **Mic** = keyboard backlight, **$** = screen frontlight Off/Low/Med/High |
| Anywhere | **Alt+H** home, **Alt+L** lock, **Alt+R** clean e-ink refresh |
| Side rocker | **up**: tap = lock/unlock, hold 3 s = power off; **down** = reset |
| Contacts | **Space** cycles filters, **F** find |
| Channels | **N** new (`#hashtag`, name + key, or `public`), **I** show key, **X** delete |

Destructive actions (delete, clear, reboot, power off) need **Y** — Enter never confirms them.

## Building from source

Requirements: Python 3, Git and PlatformIO (`python -m pip install platformio esptool`).

```powershell
maxmesh\tools\build_flash.ps1            # build + flash app over USB (keeps data)
maxmesh\tools\build_flash.ps1 -Erase     # factory reset + full flash
maxmesh\tools\make_release.ps1           # build release files into maxmesh\release\
```

PlatformIO environment: `LilyGo_TDeck_Max_standalone_ble`. The version is `MAXMESH_VERSION` in
`variants/lilygo_tdeck_max/platformio.ini`.

| Path | What |
|---|---|
| `variants/lilygo_tdeck_max/` | board port: XL9555 power rails, SX1262 on the shared SPI bus, GDEQ031T10 e-paper, TCA8418 keyboard, CST3530 touch keys, BQ27220 gauge, DRV2605 haptics, light sleep |
| `examples/companion_radio/ui-max/` | the on-device UI, message store, spell checker, word list |
| `examples/companion_radio/MyMesh.*` | small upstream additions: `ui*` helpers + `onSignedMessageRecv` hook |
| `boards/t-deck-max.json` | PlatformIO board definition |
| `maxmesh/tools/` | build/flash/release scripts, `import_json.py` (channels + favourites from a MeshCore app export), `maxctl.py` (serial test console with screenshots), `make_dict.py` |
| `maxmesh/dict/` | source word list for the spell checker |

## Not done yet

4G (SMS/calls), speaker/microphone, IMU, SD card, map view, trace route, telemetry requests, WiFi.
Built-in radio presets: USA/Canada and EU/UK Narrow (set others manually).

## Credits & licences

- **MeshCore** — © Scott Powell / rippleradios.com, MIT licence (see `license.txt`). maxmesh additions
  are released under the same MIT terms.
- **GxEPD2** (e-paper driver) — Jean-Marc Zingg, **GPL-3.0**. Firmware binaries that include it are
  therefore distributed under GPL-3.0 terms; the complete source is this repository.
- **RadioLib** — Jan Gromeš, MIT. **Adafruit GFX** — BSD.
- **Spell-check word list** — SCOWL / English Speller Database by Kevin Atkinson
  (<https://wordlist.aspell.net>), permissive licence (full notice in `maxmesh/dict/scowl-50-en_US.txt`).
- **LilyGo T-Deck-MAX** examples and **Meck** (MIT, pelgraine/Meck) were used as hardware references
  (pin map, keyboard scan codes, touch-controller protocol).
