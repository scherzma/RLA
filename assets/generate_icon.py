"""Regenerate the SVG, preview and Windows icon. Requires Pillow."""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
SCALE = 4
# Two time-shifted movement traces, using the application's A/B colors.
TRACES = [
    ("#48B7FF", [(40, 143), (58, 143), (70, 137), (92, 64), (101, 64),
                  (129, 177), (138, 177), (154, 132), (166, 124), (216, 124)]),
    ("#FF9463", [(40, 170), (86, 170), (98, 163), (120, 90), (129, 90),
                  (157, 203), (166, 203), (182, 158), (194, 150), (216, 150)]),
]

svg = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">',
       '<rect x="8" y="8" width="240" height="240" rx="52" fill="#142236" stroke="#33465F" stroke-width="2"/>']
im = Image.new("RGBA", (256 * SCALE, 256 * SCALE))
draw = ImageDraw.Draw(im)
draw.rounded_rectangle((8*SCALE, 8*SCALE, 248*SCALE, 248*SCALE),
                       radius=52*SCALE, fill="#142236", outline="#33465F", width=2*SCALE)
for color, points in TRACES:
    svg.append(f'<polyline points="{" ".join(f"{x},{y}" for x, y in points)}" '
               f'fill="none" stroke="{color}" stroke-width="13" stroke-linecap="round" stroke-linejoin="round"/>')
    scaled = [(x*SCALE, y*SCALE) for x, y in points]
    draw.line(scaled, fill=color, width=13*SCALE, joint="curve")
    r = 6.5*SCALE
    for x, y in scaled:
        draw.ellipse((x-r, y-r, x+r, y+r), fill=color)
svg.append('</svg>')
(ROOT / "rla.svg").write_text("\n".join(svg) + "\n", encoding="utf-8")
im = im.resize((256, 256), Image.Resampling.LANCZOS)
im.save(ROOT / "rla.png")
im.save(ROOT / "rla.ico", sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
