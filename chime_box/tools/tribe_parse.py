# Parse the presets table out of the sequencer page as TEXT (nothing from it is executed).
import re, json, sys

src = open(sys.argv[1], encoding="utf-8").read()
blk = src[src.index("const presets ="):]
blk = blk[:blk.index("\n};") + 3]
STR = r"'((?:[^'\\]|\\.)*)'"


def unq(s):
    return json.loads('"' + s.replace("\\'", "'").replace('"', '\\"') + '"') if s is not None else ""


def get(pat, body):
    m = re.search(pat, body, re.S)
    return m.group(1) if m else None


out = {}
for m in re.finditer(r"\n  '([a-z0-9-]+)': \{(.*?)\n  \},?", blk, re.S):
    key, body = m.group(1), m.group(2)
    pat = {r: [int(x) for x in v.replace(" ", "").replace("\n", "").split(",") if x]
           for r, v in re.findall(r"(\w+):\s*\[([0-9,\s]+)\]", body)}
    out[key] = {"name": unq(get("name: " + STR, body)), "timeSig": get(r"timeSig: '([^']+)'", body),
                "bpm": int(get(r"bpm: (\d+)", body)), "swing": int(get(r"swing: (\d+)", body)),
                "desc": unq(get("desc: " + STR, body)), "pattern": pat}

json.dump(out, open(sys.argv[2], "w", encoding="utf-8"), indent=1, ensure_ascii=False)
for k, p in out.items():
    lens = sorted({len(v) for v in p["pattern"].values()})
    print(f"{k:24} {p['timeSig']:5} {p['bpm']:4} sw{p['swing']:<3} steps{lens} {','.join(p['pattern'])}")
print(len(out), "presets")
