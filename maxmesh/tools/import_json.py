"""
import_json.py - copy channels and FAVOURITE contacts from a MeshCore app config export
(…_meshcore_config_….json) onto the T-Deck MAX over USB.

  python import_json.py <export.json> [--dry-run]

Only 'channels' (except "Public", which the device already has) and contacts whose
favourite flag is set are sent. The export's private key and other settings are ignored.
"""
import json
import sys
import time

from maxctl import open_port, send, read_until


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    dry = "--dry-run" in sys.argv
    with open(sys.argv[1], encoding="utf-8") as f:
        cfg = json.load(f)

    cmds = []
    for ch in cfg.get("channels", []):
        name, secret = (ch.get("name") or "").strip(), (ch.get("secret") or "").strip()
        if not name or name == "Public" or len(secret) not in (32, 64):
            continue
        cmds.append(("channel " + name, "!addch %s %s" % (secret, name[:31])))

    for c in cfg.get("contacts", []):
        if not (int(c.get("flags") or 0) & 1):
            continue
        key = (c.get("public_key") or "").strip()
        if len(key) != 64:
            continue
        name = (c.get("custom_name") or c.get("name") or key[:8]).strip()[:31]
        lat, lon = float(c.get("latitude") or 0), float(c.get("longitude") or 0)
        cmds.append(("favourite " + name, "!addct %d %s %.6f %.6f %s" % (int(c.get("type") or 1), key, lat, lon, name)))

    if dry:
        for label, _ in cmds:
            print("would add", label)
        return

    s = open_port()
    for label, cmd in cmds:
        send(s, cmd)
        resp = read_until(s, lambda l: l.startswith("OK") or l.startswith("ERR"), 4)
        print("%-32s %s" % (label, resp[-1] if resp else "no reply"))
        time.sleep(0.15)
    s.close()


if __name__ == "__main__":
    main()
