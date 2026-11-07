#!/usr/bin/env python3
"""make_dmg.py APP BANNER OUT.dmg VOLUME_NAME: read-only DMG with the banner, the app and an Applications link.
The Finder layout is written directly with dmgbuild, so no Finder automation or permission prompt is involved."""
import sys
from pathlib import Path
import dmgbuild
app, banner, out, volume = sys.argv[1:5]
name = Path(app).name
settings = {
    "format": "UDZO",
    "compression_level": 9,
    "filesystem": "HFS+",
    "files": [app],
    "symlinks": {"Applications": "/Applications"},
    "background": banner,
    "show_status_bar": False,
    "show_tab_view": False,
    "show_toolbar": False,
    "show_pathbar": False,
    "show_sidebar": False,
    "window_rect": ((120, 120), (640, 400)),
    "default_view": "icon-view",
    "arrange_by": None,
    "icon_size": 112,
    "text_size": 13,
    "icon_locations": {name: (170, 190), "Applications": (450, 190)},
}
dmgbuild.build_dmg(out, volume, settings=settings)
