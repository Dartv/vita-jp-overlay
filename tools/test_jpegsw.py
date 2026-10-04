#!/usr/bin/env python3
"""Round-trips host/samples/*.jpg through the software encoder (vjo-jpegsw)
and reports PSNR against the source; the output must decode with Pillow."""
import math, os, subprocess, sys, tempfile
from PIL import Image

root = os.path.join(os.path.dirname(__file__), "..")
tool = os.path.join(root, "build", "host", "vjo-jpegsw")
worst = 99.0
for name in sorted(os.listdir(os.path.join(root, "host", "samples"))):
    if not name.endswith(".jpg"):
        continue
    src = Image.open(os.path.join(root, "host", "samples", name)).convert("RGBA")
    w, h = src.size
    with tempfile.TemporaryDirectory() as d:
        raw, out = os.path.join(d, "in.rgba"), os.path.join(d, "out.jpg")
        open(raw, "wb").write(src.tobytes())
        subprocess.run([tool, raw, str(w), str(h), out, "90"], check=True)
        dec = Image.open(out).convert("RGB")
        dec.load()
    a, b = src.convert("RGB").tobytes(), dec.tobytes()
    mse = sum((x - y) ** 2 for x, y in zip(a, b)) / len(a)
    psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
    worst = min(worst, psnr)
    print(f"{name}: {w}x{h} PSNR {psnr:.1f} dB")
sys.exit(0 if worst > 30 else 1)
