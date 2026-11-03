"""Writes docs/images/move-both-mice.gif.
Needs: pip install playwright pillow; playwright install chromium.
Usage: python move_gif.py [output-folder]"""
import io, math, os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..', 'docs', 'images')
BG, PANEL, LINE = "#0f1115", "#171b24", "#2a3040"
TXT, DIM, GOLD = "#e3e6ee", "#8a93a6", "#ffd36b"
A, B = "#3399ff", "#ff7a3d"
FONT = "font-family='Segoe UI, Helvetica, Arial, sans-serif'"
W, H = 760, 320
N, CYCLES = 72, 2   # frames per loop, back-and-forth cycles per loop (seamless loop)
LAG = 0.05         # B delay as a fraction of one cycle (exaggerated)

def speed(t): return abs(math.cos(2 * math.pi * t))

def mouse(cx, cy, color, label):
    body = "M-23,-28 C-23,-44 -13,-48 0,-48 C13,-48 23,-44 23,-28 L23,24 C23,40 13,48 0,48 C-13,48 -23,40 -23,24 Z"
    return (f"<g transform='translate({cx:.1f},{cy}) scale(1.12)'>"
            f"<path d='{body}' fill='#202636' stroke='{color}' stroke-width='2.4'/>"
            f"<line x1='0' y1='-48' x2='0' y2='-16' stroke='{color}' stroke-opacity='0.7' stroke-width='1.4'/>"
            f"<rect x='-3' y='-40' width='6' height='13' rx='3' fill='{color}'/>"
            f"<text x='0' y='26' text-anchor='middle' fill='{color}' font-size='18' font-weight='700'>{label}</text></g>")

def grip(cx, cy):
    # One hand holds both mice: drawn as a shared grip around them, no hand.
    return (f"<rect x='{cx - 64:.1f}' y='{cy - 66}' width='128' height='132' rx='30' fill='{GOLD}' fill-opacity='0.06' "
            f"stroke='{GOLD}' stroke-opacity='0.7' stroke-width='1.5' stroke-dasharray='5 4'/>"
            f"<text x='{cx:.1f}' y='{cy + 84}' text-anchor='middle' fill='{GOLD}' font-size='11'>one hand</text>")

def squeeze(cx, cy, k):
    o = 3 * abs(math.sin(2 * math.pi * k / 12))
    out = [f"<g fill='{GOLD}' stroke='{GOLD}' stroke-width='2.2' stroke-linecap='round'>"]
    for side in (-1, 1):
        tip = cx + side * (70 - o)
        out.append(f"<line x1='{tip + side * 18:.1f}' y1='{cy}' x2='{tip + side * 6:.1f}' y2='{cy}'/>"
                   f"<path d='M{tip:.1f},{cy} l{side * 9},-6 v12 z' stroke='none'/>")
    out.append(f"<text x='{cx - 88:.1f}' y='{cy - 12}' text-anchor='middle' stroke='none' font-size='11'>press</text>"
               f"<text x='{cx + 88:.1f}' y='{cy - 12}' text-anchor='middle' stroke='none' font-size='11'>press</text></g>")
    return "".join(out)

def frame(k):
    t = k / N * CYCLES
    s = [f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {W} {H}' width='{W}' height='{H}'>",
         f"<rect width='{W}' height='{H}' fill='{BG}'/>", f"<g {FONT} font-size='13' fill='{TXT}'>"]
    # Left panel: hand and mice.
    s.append(f"<rect x='16' y='16' width='344' height='288' rx='12' fill='{PANEL}' stroke='{LINE}'/>")
    s.append(f"<circle cx='41' cy='37' r='10' fill='{GOLD}'/><text x='41' y='41.5' text-anchor='middle' fill='{BG}' font-size='12' font-weight='700'>1</text><text x='58' y='42' font-size='14' font-weight='600'>Hold, press, move</text>")
    s.append(f"<text x='58' y='60' fill='{DIM}' font-size='12'>Both mice in one hand, pressed together</text>")
    x = 188 + 52 * math.sin(2 * math.pi * t)
    s.append(f"<g stroke='{DIM}' fill='{DIM}' stroke-width='1.5'><line x1='72' y1='284' x2='304' y2='284'/>"
             f"<path d='M66,284 l9,-5 v10 z M310,284 l-9,-5 v10 z' stroke='none'/></g>")
    s.append(f"<text x='188' y='276' text-anchor='middle' fill='{DIM}' font-size='11'>move back and forth</text>")
    s.append(grip(x, 146))
    s.append(mouse(x - 27, 146, A, "A"))
    s.append(mouse(x + 27, 146, B, "B"))
    s.append(squeeze(x, 146, k))
    # Right panel: what each mouse reports.
    L, R, T, Bo = 400, 728, 92, 262
    s.append(f"<rect x='376' y='16' width='368' height='288' rx='12' fill='{PANEL}' stroke='{LINE}'/>")
    s.append(f"<circle cx='401' cy='37' r='10' fill='{GOLD}'/><text x='401' y='41.5' text-anchor='middle' fill='{BG}' font-size='12' font-weight='700'>2</text><text x='418' y='42' font-size='14' font-weight='600'>What RLA records</text>")
    s.append(f"<text x='418' y='60' fill='{DIM}' font-size='12'>Movement speed from each mouse, same clock</text>")
    for gy in (T, (T + Bo) / 2):
        s.append(f"<line x1='{L}' y1='{gy}' x2='{R}' y2='{gy}' stroke='{LINE}' stroke-dasharray='2 4'/>")
    s.append(f"<line x1='{L}' y1='{Bo}' x2='{R}' y2='{Bo}' stroke='{LINE}'/>")
    span = 1.5
    def y(v): return Bo - (Bo - T) * v
    def xt(tt): return L + (R - L) * (tt - (t - span)) / span
    for color, lag in ((A, 0), (B, LAG)):
        pts = " ".join(f"{L + (R - L) * i / 240:.1f},{y(speed(t - span + span * i / 240 - lag)):.1f}" for i in range(241))
        s.append(f"<polyline points='{pts}' fill='none' stroke='{color}' stroke-width='2.6' stroke-linejoin='round'/>")
        s.append(f"<circle cx='{R}' cy='{y(speed(t - lag)):.1f}' r='4.5' fill='{color}'/>")
    # B-A marker on the most recent complete peak.
    peak = math.floor((t - LAG) * 2) / 2
    if xt(peak) > L + 30:
        xa, xb, yy = xt(peak), xt(peak + LAG), T - 12
        s.append(f"<g stroke='{GOLD}' stroke-width='2'><line x1='{xa:.1f}' y1='{yy}' x2='{xb:.1f}' y2='{yy}'/>"
                 f"<line x1='{xa:.1f}' y1='{yy - 5}' x2='{xa:.1f}' y2='{yy + 5}'/><line x1='{xb:.1f}' y1='{yy - 5}' x2='{xb:.1f}' y2='{yy + 5}'/></g>"
                 f"<text x='{(xa + xb) / 2:.1f}' y='{yy - 8}' text-anchor='middle' fill='{GOLD}' font-size='12' font-weight='600'>B−A</text>")
    s.append(f"<g font-size='12'><rect x='{L}' y='{Bo + 14}' width='10' height='10' rx='2' fill='{A}'/><text x='{L + 16}' y='{Bo + 23}'>A</text>"
             f"<rect x='{L + 44}' y='{Bo + 14}' width='10' height='10' rx='2' fill='{B}'/><text x='{L + 60}' y='{Bo + 23}'>B (later)</text>"
             f"<text x='{R}' y='{Bo + 23}' text-anchor='end' fill='{DIM}' font-size='11'>time →</text></g>")
    s.append("</g></svg>")
    return "".join(s)

if __name__ == '__main__':
    from PIL import Image
    from playwright.sync_api import sync_playwright
    frames = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={'width': W, 'height': H})
        for k in range(N):
            page.set_content(f"<html><body style='margin:0;background:{BG}'>{frame(k)}</body></html>")
            frames.append(Image.open(io.BytesIO(page.screenshot())).convert('RGB'))
        browser.close()
    sheet = Image.new('RGB', (W, H * 4))
    for i, k in enumerate((0, N // 4, N // 2, 3 * N // 4)): sheet.paste(frames[k], (0, H * i))
    palette = sheet.quantize(colors=48, method=Image.Quantize.MEDIANCUT)
    gif = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
    gif[0].save(os.path.join(OUT, 'move-both-mice.gif'), save_all=True, append_images=gif[1:], duration=40, loop=0, optimize=True)
