"""Generate the HardlyAFK iOS icon set. Requires Pillow (`python -m pip install pillow`)."""
from pathlib import Path
import json
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
ICONS = ROOT / "app/Assets.xcassets/AppIcon.appiconset"
MANIFEST = json.loads((ICONS / "Contents.json").read_text())

size = 1024
image = Image.new("RGB", (size, size), "#0b1020")
draw = ImageDraw.Draw(image)
draw.rounded_rectangle((90, 90, 934, 934), radius=150, fill="#181f37", outline="#586d9c", width=14)
draw.rounded_rectangle((151, 204, 873, 738), radius=70, fill="#0b1424", outline="#586d9c", width=12)
draw.rounded_rectangle((151, 204, 873, 312), radius=65, fill="#293956")
draw.rectangle((151, 275, 873, 312), fill="#293956")
for x, color in [(217, "#e48692"), (268, "#ead493"), (319, "#91d5b4")]:
    draw.ellipse((x - 17, 248 - 17, x + 17, 248 + 17), fill=color)
# A clean terminal prompt large enough to remain legible at notification size.
draw.line([(259, 428), (347, 487), (259, 546)], fill="#a7eacc", width=32, joint="curve")
draw.rounded_rectangle((390, 531, 602, 560), radius=12, fill="#a7eacc")
font_path = ROOT / "app/terminal/JetBrainsMonoNerdFontMono-Bold.ttf"
font = ImageFont.truetype(str(font_path), 98)
draw.text((512, 822), "AFK", font=font, anchor="mm", fill="#e5ecfa", stroke_width=0)

for icon in MANIFEST["images"]:
    points = float(icon["size"].split("x")[0])
    pixels = round(points * int(icon["scale"].removesuffix("x")))
    image.resize((pixels, pixels), Image.Resampling.LANCZOS).save(ICONS / icon["filename"], optimize=True)
print(f"Generated {len(MANIFEST['images'])} HardlyAFK app icons from a 1024px source")
