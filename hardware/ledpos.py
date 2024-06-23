#!/usr/bin/env python3

from math import *

circle_x = 110
circle_y = 160
circle_r = 40 + 1.27 # half of LED width wider

nleds = 11

angle_step = pi / nleds
phi = angle_step / 2

print('LEDs:')
for i in range(nleds):
    x = circle_x - circle_r * cos(phi)
    y = circle_y - circle_r * sin(phi)
    print('    (at {0:.2f} {1:.2f} {2:.2f})'.format(x, y, 360 - 180 * phi / pi))
    phi += angle_step

center_x = 110
center_y = 160
r = 50
psi = 0
nsteps = 2 * nleds
delta_psi = pi / nsteps

print('Zone:')
for i in range(nsteps + 1):
    x = center_x - r * cos(psi)
    y = center_y - r * sin(psi)
    print('        (xy {0:.1f} {1:.1f})'.format(x, y))
    psi += delta_psi
