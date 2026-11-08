# Render one fixed line in Verna's voice with the brain's own ElevenLabs settings (key read from brain.json, never printed).
import sys
sys.path.insert(0, r"C:\Users\mark\Documents\Ragamuffin Workshop\brain\panel")
import design_verna as dv
text, out = sys.argv[1], sys.argv[2]
c = dv.cfg()
settings = c.get("elevenlabs_settings") or {"stability": 0.5, "similarity_boost": 0.8, "style": 0.2, "use_speaker_boost": True, "speed": 1.04}
pcm = dv.call(f"/v1/text-to-speech/{c['elevenlabs_voice_id']}?output_format=pcm_16000",
              {"text": text, "model_id": c.get("elevenlabs_model", "eleven_flash_v2_5"), "voice_settings": settings}, raw=True)
open(out, "wb").write(pcm)
print(f"{out}: {len(pcm)} bytes, {len(pcm) / 32000:.1f} s")
