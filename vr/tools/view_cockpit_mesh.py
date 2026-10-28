"""Offline, two-sided geometry view; orange lines mark welded open edges."""
import collections
import pathlib
import numpy as np
from PIL import Image, ImageDraw
from inspect_cockpit_gaps import interiors


def render(car):
    w, h = 1400, 900
    image = np.full((h, w, 3), (110, 160, 200), dtype=np.uint8)
    depth = np.full((h, w), np.inf)
    eye = np.array((.05, .42, 1.0 if car == 'CTS' else .90))
    forward = np.array((1., 0., -.24))
    forward /= np.linalg.norm(forward)
    right = np.array((0., -1., 0.))
    up = np.cross(right, forward)
    lines = []
    for name, vertices, triangles in interiors(car, lambda n: n.endswith('_A') and
                                             ('KIT00_BODY' in n or 'KIT00_HOOD' in n or 'INTERIOR' in n)):
        v = np.array(vertices)
        relative = v - eye
        z = relative @ forward
        screen = np.stack((w / 2 + 800 * (relative @ right) / z,
                           h / 2 - 800 * (relative @ up) / z), axis=1)
        interior = 'INTERIOR' in name
        welded = [tuple(np.round(p, 4)) for p in v]
        edges = collections.Counter(tuple(sorted((welded[a], welded[b])))
                                    for t in triangles for a, b in zip(t, t[1:] + t[:1]) if welded[a] != welded[b])
        for t in triangles:
            if min(z[list(t)]) < .04:
                continue
            p = screen[list(t)]
            x0, y0 = np.maximum(np.floor(p.min(axis=0)).astype(int), 0)
            x1, y1 = np.minimum(np.ceil(p.max(axis=0)).astype(int), (w-1, h-1))
            if x1 < x0 or y1 < y0:
                continue
            a, b, c = p
            denominator = (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
            if abs(denominator) < 1e-8:
                continue
            yy, xx = np.mgrid[y0:y1+1, x0:x1+1]
            u = ((b[0]-xx)*(c[1]-yy)-(b[1]-yy)*(c[0]-xx))/denominator
            q = ((c[0]-xx)*(a[1]-yy)-(c[1]-yy)*(a[0]-xx))/denominator
            r = 1-u-q
            d = 1 / (u/z[t[0]] + q/z[t[1]] + r/z[t[2]])
            dest = depth[y0:y1+1, x0:x1+1]
            mask = (u>=0)&(q>=0)&(r>=0)&(d<dest)
            normal = np.cross(v[t[1]]-v[t[0]], v[t[2]]-v[t[0]])
            normal /= max(np.linalg.norm(normal), 1e-12)
            shade = .45 + .55*abs(normal @ np.array((.3, -.2, .932)))
            color = np.array((130, 130, 130) if interior else (70, 130, 90)) * shade
            image[y0:y1+1, x0:x1+1][mask] = color.astype(np.uint8)
            dest[mask] = d[mask]
        if interior:
            for t in triangles:
                for a, b in zip(t, t[1:] + t[:1]):
                    if edges[tuple(sorted((welded[a], welded[b])))] == 1 and min(z[a], z[b])>.04:
                        lines.append((screen[a], screen[b], z[a], z[b]))
    result = Image.fromarray(image)
    draw = ImageDraw.Draw(result)
    for a, b, za, zb in lines:
        for f in np.linspace(0, 1, max(2, min(2000, int(np.linalg.norm(b-a))))):
            p = a*(1-f)+b*f
            x, y = int(p[0]), int(p[1])
            d = 1/((1-f)/za+f/zb)
            if 0<=x<w and 0<=y<h and d<=depth[y,x]+.006:
                draw.point((x,y), fill=(255,160,30))
    draw.text((12,12), car+' original geometry: interior gray; body/hood green; open edges orange', fill='white')
    result.save(pathlib.Path(__file__).resolve().parents[1] / (car+'-geometry.png'))


if __name__ == '__main__':
    for car in ('MURCIELAGO', 'CTS', 'CORVETTEC6R'):
        render(car)