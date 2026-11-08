# Build chime_box/presets.cpp from the Tribe of Abraham drum sequencer's presets (MIT, parsed as text by tribe_parse.py).
import json, sys

src, out = sys.argv[1], sys.argv[2]
P = json.load(open(src, encoding="utf-8"))
ROLES = ["", "kick", "snare", "hihat", "open", "rimshot", "clap", "tomlo", "tomhi", "cowbell", "shaker"]   # = DrumRole
TS = ["4/4", "3/4", "6/8", "5/4", "7/8", "9/8", "12/8"]                                                 # = TIME_SIGS order


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').encode("ascii", "backslashreplace").decode().replace("\\u", "\\u") + '"'


def ascii_text(s):  # the screen font is ASCII; keep the page text close but plain
    rep = {"—": "-", "–": "-", "’": "'", "ã": "a", "ä": "a", "é": "e", "ş": "s", "ı": "i",
           "ā": "a", "→": "->"}
    return "".join(rep.get(ch, ch) for ch in s).encode("ascii", "ignore").decode()


lines = ["// presets.cpp: GENERATED. World-rhythm presets from the Tribe of Abraham drum sequencer",
         "// (https://github.com/tribeofabraham/drum-sequencer, MIT License, Copyright (c) Tribe of Abraham).",
         "// Levels: 0 off, 1 ghost, 2 medium, 3 accent. Regenerate with tribe_parse + mkpresets.",
         '#include "chimes.h"', ""]
entries = []
for key, p in P.items():
    if key == "empty":
        continue
    rows = [(r, v) for r, v in p["pattern"].items() if r in ROLES]
    steps = max(len(v) for _, v in rows)
    flat = []
    for _, v in rows:
        flat += (v + [0] * steps)[:steps]
    var = "lv_" + key.replace("-", "_")
    lines.append(f"static const uint8_t {var}[{len(flat)}] = {{{','.join(map(str, flat))}}};")
    roles = ",".join(str(ROLES.index(r)) for r, _ in rows)
    entries.append(f"  {{{cstr(ascii_text(p['name']))}, {cstr(ascii_text(p['desc']))}, {TS.index(p['timeSig'])}, {p['bpm']}, "
                   f"{p['swing']}, {len(rows)}, {steps}, {{{roles}}}, {var}}},")
lines.append("")
lines.append("const DrumPreset PRESETS[] = {")
lines += entries
lines.append("};")
lines.append("const int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);")
open(out, "w").write("\n".join(lines) + "\n")
print(len(entries), "presets ->", out)
