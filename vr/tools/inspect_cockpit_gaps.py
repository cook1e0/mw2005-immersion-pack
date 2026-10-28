"""Read uncompressed MW geometry chunks and report welded interior boundary loops."""
import collections
import math
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[4]


def chunks(data, start=0, end=None):
    end = len(data) if end is None else end
    while start + 8 <= end:
        tag, size = struct.unpack_from('<II', data, start)
        stop = start + 8 + size
        if stop > end:
            raise ValueError('invalid chunk size')
        yield tag, start + 8, stop
        start = stop


def interiors(car, include=None):
    data = (ROOT / 'CARS' / car / 'GEOMETRY.BIN').read_bytes()
    for tag, begin, end in chunks(data):
        if tag != 0x80134000:
            continue
        for tag, begin, end in chunks(data, begin, end):
            if tag != 0x80134010:
                continue
            parts = list(chunks(data, begin, end))
            header = next((data[a:b] for t, a, b in parts if t == 0x134011), b'')
            name = header[160:].split(b'\0')[0].decode('ascii', errors='replace')
            if not (include(name) if include else '_INTERIOR_A' in name):
                continue
            mesh = next((list(chunks(data, a, b)) for t, a, b in parts if t == 0x80134100), [])
            payload = {}
            for t, a, b in mesh:
                # Chunk payloads may start with 0x11111111 padding words.
                if t in (0x134B01, 0x134B03):
                    while data[a:a + 4] == b'\x11' * 4:
                        a += 4
                    payload[t] = data[a:b]
            vb, ib = payload[0x134B01], payload[0x134B03]
            stride = 36  # MW car vertex: xyz, normal xyz, ARGB, uv.
            count, rem = divmod(len(vb), stride)
            if rem and vb[-rem:] != b'\x00' * rem and vb[-rem:] != b'\x11' * rem:
                raise ValueError((name, stride, len(vb), vb[-rem:]))
            vertices = [struct.unpack_from('<fff', vb, i * stride) for i in range(count)]
            triangle_count = struct.unpack_from('<I', header, 20)[0]
            indices = struct.unpack_from('<' + 'H' * (triangle_count * 3), ib)
            if not all(0 <= i < count for i in indices):
                raise ValueError((name, 'index outside vertex buffer'))
            if not all(math.isfinite(x) for v in vertices for x in v):
                raise ValueError((name, 'nonfinite vertex'))
            triangles = [indices[i:i + 3] for i in range(0, len(indices), 3)]
            yield name, vertices, triangles


def report(car):
    for name, vertices, triangles in interiors(car):
        print(name, len(vertices), 'vertices', len(triangles), 'triangles')
        welded = {}
        ids = []
        representatives = {}
        for i, v in enumerate(vertices):
            key = tuple(round(x, 4) for x in v)
            idx = welded.setdefault(key, len(welded))
            ids.append(idx)
            representatives[idx] = i
        edges = collections.Counter()
        for tri in triangles:
            a, b, c = (ids[i] for i in tri)
            if len({a, b, c}) < 3:
                continue
            for x, y in ((a, b), (b, c), (c, a)):
                edges[tuple(sorted((x, y)))] += 1
        boundary = {edge for edge, n in edges.items() if n == 1}
        adjacent = collections.defaultdict(set)
        for a, b in boundary:
            adjacent[a].add(b)
            adjacent[b].add(a)
        unseen = set(adjacent)
        while unseen:
            todo = [unseen.pop()]
            component = set(todo)
            while todo:
                for j in adjacent[todo.pop()]:
                    if j in unseen:
                        unseen.remove(j)
                        component.add(j)
                        todo.append(j)
            raw = [representatives[j] for j in component]
            pts = [vertices[j] for j in raw]
            lo = tuple(round(min(p[k] for p in pts), 3) for k in range(3))
            hi = tuple(round(max(p[k] for p in pts), 3) for k in range(3))
            print(' boundary', len(raw), 'bounds', lo, hi, 'ids', sorted(raw))


if __name__ == '__main__':
    for car in ('MURCIELAGO', 'CTS', 'CORVETTEC6R'):
        report(car)