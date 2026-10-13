#!/usr/bin/env python3
"""RDPServer: draws the icons (PNG) that the Makefile turns into !Sprites.
   tools/mkicons.py app/icons     (needs Pillow; the PNGs are kept in git)
   A monitor; 'on' has an arrow going out of it (a client is connected)."""
import sys
from PIL import Image, ImageDraw

def icon(size, connected):
    s = size / 34.0
    im = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    R = lambda *v: [round(x * s) for x in v]
    d.rectangle(R(3, 5, 30, 24), fill=(70, 70, 80, 255))            # bezel
    d.rectangle(R(5, 7, 28, 22), fill=(40, 110, 200, 255))          # screen
    d.rectangle(R(14, 24, 19, 27), fill=(70, 70, 80, 255))          # neck
    d.rectangle(R(9, 27, 24, 29), fill=(70, 70, 80, 255))           # foot
    if connected:
        d.polygon(R(17, 11, 26, 15, 17, 19), fill=(250, 180, 30, 255))   # arrow head
        d.rectangle(R(9, 13, 18, 17), fill=(250, 180, 30, 255))          # shaft
    else:
        d.rectangle(R(9, 12, 24, 13), fill=(230, 230, 240, 255))         # a window
        d.rectangle(R(9, 14, 24, 18), fill=(200, 200, 210, 255))
    return im

out = sys.argv[1]
icon(34, False).save(f'{out}/rdpserver.png')
icon(17, False).save(f'{out}/sm-rdpserver.png')
icon(34, True).save(f'{out}/rdpserver-on.png')
