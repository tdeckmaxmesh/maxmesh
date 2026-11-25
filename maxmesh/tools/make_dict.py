"""
make_dict.py - build the T-Deck MAX spell-check dictionary.

  python make_dict.py   (reads maxmesh/dict/scowl-50-en_US.txt, writes examples/companion_radio/ui-max/spell_words.txt)

Output: lowercase words, one per line, byte-sorted, so the firmware can binary-search
the embedded file directly (no index needed).
Word list: SCOWL / English Speller Database size 50, US (C) Kevin Atkinson - see
https://wordlist.aspell.net for its permissive licence.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "dict", "scowl-50-en_US.txt")
OUT = os.path.join(HERE, "..", "..", "examples", "companion_radio", "ui-max", "spell_words.txt")

# chat / mesh vocabulary that a normal dictionary flags
EXTRA = """
ok okay lol lmao lmfao rofl idk btw tbh omg imo imho rn thx ty np brb gtg afaik iirc fyi asap
irl tho thru nvm smh jk ikr wyd hbu hmu bc cuz ya yah yea yep nope nah gonna wanna gotta kinda sorta
yall dunno lemme gimme ur u im ive id ill youre theyre hes shes dont cant wont didnt isnt wasnt arent
gps lora meshcore meshtastic mesh rptr repeater repeaters tdeck deck bluetooth ble wifi app apps
ping pong ack acks advert adverts hop hops antenna antennas uplink downlink rssi snr dbm mhz khz
emoji emojis online offline login logout username password admin config wx qth qsl qrz
hi hey hiya yo howdy tx rx sos emt eta
""".split()


def main():
    words = set()
    with open(SRC, encoding="utf-8") as f:
        lines = f.read().split("\n---\n", 1)[1].splitlines()
    for w in lines:
        w = w.strip().lower()
        if not w or not re.fullmatch(r"[a-z']+", w):
            continue
        words.add(w)
        if "'" in w:
            words.add(w.replace("'", ""))     # chat-style contractions: dont, im, johns
    words.update(EXTRA)
    out = sorted(words, key=lambda s: s.encode())
    with open(OUT, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("%d words, %d bytes -> %s" % (len(out), os.path.getsize(OUT), OUT))


if __name__ == "__main__":
    main()
