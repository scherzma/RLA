"""Writes docs/images/how-it-works.svg and pipeline-vs-data-age.svg. Usage: python diagrams.py [output-folder]"""
import math, sys, os
OUT=sys.argv[1] if len(sys.argv)>1 else os.path.join(os.path.dirname(__file__),'..','..','docs','images')
BG="#0f1115"; GRID="#2a2f3a"; TXT="#d7dbe3"; DIM="#8a93a6"; A="#3399ff"; B="#ff7a3d"; OK="#ffd36b"
FONT="font-family='Segoe UI, Helvetica, Arial, sans-serif'"

# ---------- 1: how it works ----------
W,H=760,310
x0,x1,yb,amp=40,720,236,128
dx=16  # B latency in px (exaggerated)
def speed(u):  # u in [0,1] over 3.5 cycles, magnitude of back-and-forth speed
    c=3.5; a=0.75+0.25*math.sin(u*2.1+0.4)
    return abs(math.sin(math.pi*c*u))*a
def path(shift,scale=1.0):
    pts=[]
    for i in range(0,401):
        u=i/400; x=x0+u*(x1-x0-dx)+shift
        y=yb-amp*scale*speed(u)
        pts.append(f"{x:.1f},{y:.1f}")
    return "M"+" L".join(pts)
# feature positions: minima at u=k/3.5, maxima at u=(k+0.5)/3.5
def X(u,shift=0): return x0+u*(x1-x0-dx)+shift
D=8.0  # loop seconds
def op(t_on,t_off=0.97):
    # opacity animation: hidden until t_on (fraction), visible until t_off
    return (f"<animate attributeName='opacity' dur='{D}s' repeatCount='indefinite' "
            f"keyTimes='0;{t_on:.3f};{t_on+0.04:.3f};{t_off:.3f};1' values='0;0;1;1;0'/>")
L=2400
s=[f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {W} {H}' width='{W}' height='{H}' role='img' aria-label='How RLA measures relative latency'>",
   f"<rect width='{W}' height='{H}' rx='10' fill='{BG}'/>",
   f"<g {FONT} font-size='14' fill='{TXT}'>"]
# captions (three stages)
caps=[(0.0,0.30,"1  Move both mice together. RLA records both report streams on one clock."),
      (0.30,0.62,"2  Find the same features in both curves: minima, maxima, 50 % slope points."),
      (0.62,0.97,"3  B−A = median offset over the matched cycles. Here B is later.")]
for a,b,t in caps:
    s.append(f"<text x='{x0}' y='34' opacity='0'>{t}<animate attributeName='opacity' dur='{D}s' repeatCount='indefinite' keyTimes='0;{a:.3f};{a+0.03:.3f};{b-0.02:.3f};{b:.3f};1' values='0;0;1;1;0;0'/></text>")
# axes
s.append(f"<line x1='{x0}' y1='{yb}' x2='{x1}' y2='{yb}' stroke='{GRID}'/>")
s.append(f"<text x='{x1}' y='{yb+22}' text-anchor='end' fill='{DIM}' font-size='12'>time</text>")
s.append(f"<text x='{x0}' y='{yb-amp-18}' fill='{DIM}' font-size='12'>movement per time bin</text>")
# legend
s.append(f"<g font-size='12'><rect x='{x1-280}' y='{yb-amp-29}' width='10' height='10' fill='{A}'/><text x='{x1-265}' y='{yb-amp-20}'>Mouse A (reference)</text>"
         f"<rect x='{x1-130}' y='{yb-amp-29}' width='10' height='10' fill='{B}'/><text x='{x1-115}' y='{yb-amp-20}'>Mouse B (test)</text></g>")
# curves drawn progressively
for col,shift,sc in ((A,0,1.0),(B,dx,0.93)):
    s.append(f"<path d='{path(shift,sc)}' fill='none' stroke='{col}' stroke-width='2.4' stroke-linejoin='round' "
             f"stroke-dasharray='{L}' stroke-dashoffset='{L}'>"
             f"<animate attributeName='stroke-dashoffset' dur='{D}s' repeatCount='indefinite' keyTimes='0;0.25;0.97;1' values='{L};0;0;{L}'/></path>")
# features
feats=[]
for k in range(1,4): feats.append(("min",k/3.5))
for k in range(0,3): feats.append(("max",(k+0.5)/3.5))
for k in range(0,4): feats.append(("half",(k+1/6)/3.5))
for i,(kind,u) in enumerate(sorted(feats,key=lambda f:f[1])):
    t_on=0.32+i*0.025
    xa,xb=X(u),X(u,dx)
    ya=yb-amp*speed(u); yb2=yb-amp*0.93*speed(u)
    yline_top=yb-amp-2 if kind=="max" else yb-46 if kind=="min" else ya-14
    s.append(f"<g opacity='0'>{op(t_on)}"
             f"<line x1='{xa:.1f}' y1='{yline_top:.1f}' x2='{xa:.1f}' y2='{yb}' stroke='{A}' stroke-dasharray='3 3' stroke-width='1.2'/>"
             f"<line x1='{xb:.1f}' y1='{yline_top:.1f}' x2='{xb:.1f}' y2='{yb}' stroke='{B}' stroke-dasharray='3 3' stroke-width='1.2'/>"
             +(f"<circle cx='{xa:.1f}' cy='{ya:.1f}' r='4' fill='{A}'/><circle cx='{xb:.1f}' cy='{yb2:.1f}' r='4' fill='{B}'/></g>" if kind!="half" else
              f"<rect x='{xa-3.5:.1f}' y='{ya-3.5:.1f}' width='7' height='7' transform='rotate(45 {xa:.1f} {ya:.1f})' fill='{A}'/><rect x='{xb-3.5:.1f}' y='{yb2-3.5:.1f}' width='7' height='7' transform='rotate(45 {xb:.1f} {yb2:.1f})' fill='{B}'/></g>"))
    # bracket in stage 3
    yy=yline_top-6
    s.append(f"<g opacity='0' stroke='{OK}' fill='{OK}'>{op(0.64+i*0.017)}"
             f"<line x1='{xa:.1f}' y1='{yy:.1f}' x2='{xb:.1f}' y2='{yy:.1f}' stroke-width='2'/>"
             f"<line x1='{xa:.1f}' y1='{yy-4:.1f}' x2='{xa:.1f}' y2='{yy+4:.1f}' stroke-width='2'/>"
             f"<line x1='{xb:.1f}' y1='{yy-4:.1f}' x2='{xb:.1f}' y2='{yy+4:.1f}' stroke-width='2'/></g>")
s.append(f"<g opacity='0'>{op(0.84)}<rect x='{x0}' y='{yb+34}' width='300' height='30' rx='6' fill='#1d2230' stroke='{OK}'/>"
         f"<text x='{x0+12}' y='{yb+54}' fill='{OK}' font-size='14'>B−A = +0.8 ms  (spread 0.2 ms, 20 cycles)</text></g>")
s.append(f"<text x='{x1}' y='{H-12}' text-anchor='end' fill='{DIM}' font-size='11'>Offset exaggerated for illustration</text>")
s.append("</g></svg>")
open(os.path.join(OUT,'how-it-works.svg'),'w',encoding='utf-8').write("\n".join(s))

# ---------- 2: pipeline vs data age ----------
W,H=760,370
L0,R0=150,730
def tx(ms): return L0+ms/10*(R0-L0)
P=1.0
s=[f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {W} {H}' width='{W}' height='{H}' role='img' aria-label='Pipeline and data age'>",
   f"<rect width='{W}' height='{H}' rx='10' fill='{BG}'/>",
   f"<g {FONT} font-size='13' fill='{TXT}'>",
   f"<text x='20' y='30' font-size='15'>A report holds the motion of the interval before it. It arrives after the pipeline time P.</text>"]
for m in range(0,11):
    x=tx(m); s.append(f"<line x1='{x:.1f}' y1='62' x2='{x:.1f}' y2='{H-36}' stroke='{GRID}' stroke-width='1'/>")
    s.append(f"<text x='{x:.1f}' y='{H-18}' text-anchor='middle' fill='{DIM}' font-size='11'>{m}</text>")
s.append(f"<text x='{R0}' y='{H-4}' text-anchor='end' fill='{DIM}' font-size='11'>ms</text>")
def label(y,a,b):
    s.append(f"<text x='20' y='{y+4}'>{a}</text><text x='20' y='{y+20}' fill='{DIM}' font-size='11'>{b}</text>")
yA,yB,yArr,yP,yD=90,158,198,254,306
label(yA,"Mouse A","8000 Hz, T = 0.125 ms")
for k in range(80):
    a=k*0.125; s.append(f"<rect x='{tx(a)+0.6:.1f}' y='{yA-9}' width='{tx(0.125)-tx(0)-1.2:.1f}' height='18' rx='1.5' fill='{A}' fill-opacity='0.3' stroke='{A}' stroke-width='0.8'/>")
label(yB,"Mouse B","500 Hz, T = 2 ms")
s.append(f"<text x='20' y='{yArr+4}' fill='{DIM}' font-size='12'>B arrives</text>")
for k in range(4):
    a,b=2*k,2*k+2; hl=(k==1)
    s.append(f"<rect x='{tx(a)+1:.1f}' y='{yB-11}' width='{tx(b)-tx(a)-2:.1f}' height='22' rx='3' fill='{B}' fill-opacity='{0.45 if hl else 0.16}' stroke='{B}' stroke-opacity='{1 if hl else 0.5}'/>")
    s.append(f"<path d='M{tx(b):.1f},{yB+11} L{tx(b+P):.1f},{yArr-6}' stroke='{B}' stroke-opacity='{1 if hl else 0.5}' stroke-width='1.5'/>"
             f"<circle cx='{tx(b+P):.1f}' cy='{yArr}' r='4.5' fill='{B}' fill-opacity='{1 if hl else 0.5}'/>")
s.append(f"<g fill='{OK}' stroke='{OK}' font-size='11'>"
         f"<line x1='{tx(2)+1:.1f}' y1='{yB-19}' x2='{tx(4)-1:.1f}' y2='{yB-19}' stroke-width='1.2'/>"
         f"<line x1='{tx(3):.1f}' y1='{yB-23}' x2='{tx(3):.1f}' y2='{yB-15}' stroke-width='1.6'/>"
         f"<text x='{tx(3):.1f}' y='{yB-27}' text-anchor='middle' stroke='none'>mean time of this motion: 1 ms before sending (T/2)</text>"
         f"<text x='{tx(4.5)+8:.1f}' y='{(yB+yArr)/2+4:.1f}' stroke='none'>P</text></g>")
label(yP,"Pipeline","time-weighted")
label(yD,"Data age","arrival time")
s.append(f"<rect x='{tx(2+P)+1:.1f}' y='{yP-11}' width='{tx(4)-tx(2)-2:.1f}' height='22' rx='3' fill='{B}' fill-opacity='0.45' stroke='{B}'/>")
s.append(f"<rect x='{tx(4+P)-3:.1f}' y='{yD-11}' width='6' height='22' rx='2' fill='{B}' stroke='{B}'/>")
for y,end,text in ((yP,3+P,"spread over T: shift = P"),(yD,4+P,"at arrival: shift = P + T/2")):
    s.append(f"<path d='M{tx(3):.1f},{y} L{tx(end):.1f},{y}' stroke='{OK}' stroke-width='2'/>"
             f"<circle cy='{y}' r='4.5' fill='{OK}' cx='{tx(end):.1f}'><animate attributeName='cx' dur='3s' repeatCount='indefinite' keyTimes='0;0.6;1' values='{tx(3):.1f};{tx(end):.1f};{tx(end):.1f}'/></circle>"
             f"<text x='{tx(5.4):.1f}' y='{y+5}' fill='{OK}'>{text}</text>")
s.append(f"<line x1='{tx(3):.1f}' y1='{yB-15}' x2='{tx(3):.1f}' y2='{yD+16}' stroke='{OK}' stroke-dasharray='3 3' stroke-opacity='0.6'/>")
s.append(f"<text x='20' y='50' fill='{DIM}' font-size='12'>Same P for both mice here. Data age adds T/2: 1 ms for B, 0.06 ms for A, so B−A grows by about 0.94 ms.</text>")
s.append("</g></svg>")
open(os.path.join(OUT,'pipeline-vs-data-age.svg'),'w',encoding='utf-8').write("\n".join(s))
print("ok")
