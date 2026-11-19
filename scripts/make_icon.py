#!/usr/bin/env python3
"""Generate app/icon.png in the stock TrimUI / CrossMix app-icon style.

The icon is chosen from the candidates in make_icon_options.py (see also
docs/icon-options/preview-grid.png). Requires Pillow (pip install Pillow).
"""
import os
from make_icon_options import OPTIONS, OUTDIR, ROOT, render

# Currently selected icon: A (User Guide style: blue glossy tile, white disc,
# Material "tune" glyph). Change this key to switch to another candidate.
CHOSEN = 'A'


def main():
    Option = next(o for o in OPTIONS if o[0] == CHOSEN)
    key, label_text, top, bottom, cp, gcol, disc, accent, gpx = Option
    image = render(top, bottom, cp, gcol, disc, accent, gpx)

    out = os.path.join(ROOT, 'app', 'icon.png')
    image.save(out)
    print(out, os.path.getsize(out), 'bytes')

    # Keep the prebuilt package in sync when it exists.
    dist = os.path.join(ROOT, 'dist', 'OverLyder', 'icon.png')
    if os.path.isdir(os.path.dirname(dist)):
        image.save(dist)
        print(dist, os.path.getsize(dist), 'bytes')


if __name__ == '__main__':
    main()
