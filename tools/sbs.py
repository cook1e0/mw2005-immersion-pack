# Combine vr_dump_NNN_left/right.bmp into a side-by-side PNG: sbs.py GAMEDIR INDEX OUT.png
import sys
from PIL import Image
d, i, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
a = Image.open('%s/vr_dump_%03d_left.bmp' % (d, i)).convert('RGB')
b = Image.open('%s/vr_dump_%03d_right.bmp' % (d, i)).convert('RGB')
c = Image.new('RGB', (a.width * 2 + 8, a.height), (255, 0, 255))
c.paste(a, (0, 0)); c.paste(b, (a.width + 8, 0)); c.save(out)
