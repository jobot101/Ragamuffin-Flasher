# Build chime_box/verna_lines.cpp from Verna's rendered prompts (16 kHz 16-bit PCM files from render_line.py).
# Trims silence, leaves 10% headroom, writes preview WAVs, stores samples as 8-bit mu-law (G.711) to halve the flash.
import sys, wave, array, os

D = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(D, "verna_lines.cpp")
WORDS = ["one", "two", "three", "four", "five", "six", "seven", "eight"]
LINES = [  # enum name, file key, text  (order = the VernaLine enum in chimes.h)
    ("V_READY", "ready", "Chimes ready."),
    ("V_NO_POWER", "no_power", "No power to the chimes. Plug in the power supply, then try again."),
    ("V_POWER_BACK", "power_back", "Power's on. Ring away."),
    ("V_MUTED", "muted", "The chimes are muted. Unmute them first."),
    ("V_UNMUTED", "unmuted", "Chimes on."),
    ("V_PANIC", "panic", "All stopped."),
    ("V_NO_DRIVER", "no_driver", "I can't reach that driver board. Check the cable to the chimes."),
    ("V_TOO_HOT", "too_hot", "That one's running hot. Give it a minute to cool."),
    ("V_NOT_SET_UP", "not_set_up", "That one isn't set up yet."),
    ("V_UPDATING", "updating", "Updating the chimes. Back in a moment."),
    ("V_TUNE_ON", "tune_on", "Right on."),
    ("V_TUNE_TOUCH_SHARP", "tune_touch_sharp", "That one's a touch sharp."),
    ("V_TUNE_TOUCH_FLAT", "tune_touch_flat", "That one's a touch flat."),
    ("V_TUNE_SHARP", "tune_sharp", "That one's sharp."),
    ("V_TUNE_FLAT", "tune_flat", "That one's flat."),
    ("V_TUNE_NO_SIGNAL", "tune_no_signal", "I couldn't hear that one. Check the pickup."),
    ("V_CAL_MIN", "cal_min", "Listen for the first clean strike, then tap min."),
    ("V_CAL_FULL", "cal_full", "Now tap where it stops getting louder."),
    ("V_CAL_DONE", "cal_done", "Sweep done."),
    ("V_PATTERN_SAVED", "pattern_saved", "Pattern saved."),
] + [(f"V_BOARD_BACK_{n}", f"board_back_{n}", f"Driver board {WORDS[n - 1]} is back.") for n in range(1, 9)] + [
    ("V_WEAR_CHECK", "wear_check", "That one's due for a check."),
    ("V_REC_START", "rec_start", "Recording. Play your phrase."),
    ("V_REC_SAVED", "rec_saved", "Got it. That's in the pattern."),
    ("V_REC_STOP", "rec_stop", "Recording stopped."),
]


def mulaw(x):
    BIAS, CLIP = 0x84, 32635
    sign = 0x80 if x < 0 else 0
    x = min(abs(x), CLIP) + BIAS
    exp = 0
    for e in range(7, -1, -1):
        if x & (0x80 << e):
            exp = e
            break
    man = (x >> (exp + 3)) & 0x0F
    return ~(sign | (exp << 4) | man) & 0xFF


out = ["// verna_lines.cpp: GENERATED from Verna's voice (the Oracle operator; ElevenLabs with the brain's design_verna settings).",
       "// 16 kHz mono, 8-bit mu-law (G.711). Regenerate with the render_line + mkverna tools; never hand-edit the sample data.",
       '#include "chimes.h"', ""]
total = 0
for name, key, text in LINES:
    a = array.array("h", open(os.path.join(D, key + ".pcm"), "rb").read())
    thr, s, e = 300, 0, len(a)
    while s < e and abs(a[s]) < thr: s += 1
    while e > s and abs(a[e - 1]) < thr: e -= 1
    s, e = max(0, s - 640), min(len(a), e + 640)
    a = array.array("h", (int(x * 0.9) for x in a[s:e]))
    w = wave.open(os.path.join(D, key + ".wav"), "wb")
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(a.tobytes()); w.close()
    u = bytes(mulaw(x) for x in a)
    total += len(u)
    body = ",\n".join(",".join(str(b) for b in u[i:i + 32]) for i in range(0, len(u), 32))
    out.append(f"static const uint8_t ulaw_{key}[{len(u)}] = {{\n{body}\n}};")
    print(f"{key:17} {len(u) / 16000:.2f} s  {text}")
out.append("const VernaClip VERNA[V_COUNT] = {")
for name, key, text in LINES:
    out.append(f'  {{"{key}", "{text}", ulaw_{key}, sizeof(ulaw_{key})}},   // {name}')
out.append("};")
open(OUT, "w").write("\n".join(out) + "\n")
print(f"{len(LINES)} prompts, {total / 16000:.1f} s, {total // 1024} KB of flash")
print("enum:", ", ".join(n for n, _, _ in LINES))
