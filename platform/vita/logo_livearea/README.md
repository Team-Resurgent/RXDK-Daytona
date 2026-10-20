# PS Vita icon and LiveArea images

Put your own images in this folder before running `scripts/build_vita.py`. Every
file below that is present goes into the VPK; a missing one keeps the Vita's default
(no file at all: the default icon and LiveArea). The game's name on the home screen
is always "Daytona USA", whatever the build options.

| File name (exact) | Size (pixels) | Shown as | In the VPK |
| --- | --- | --- | --- |
| `icon0.png` | 128 x 128 | home-screen bubble | `sce_sys/icon0.png` |
| `bg.png` | 840 x 500 | LiveArea background | `sce_sys/livearea/contents/bg.png` |
| `startup.png` | 280 x 158 | LiveArea "start" gate | `sce_sys/livearea/contents/startup.png` |
| `template.xml` | - | LiveArea layout (optional) | `sce_sys/livearea/contents/template.xml` |

Format: **PNG, 8-bit indexed (256-colour palette)**, exactly the size above. The
Vita refuses or blanks other PNGs (24/32-bit, wrong size), so the build stops with
the reason instead. To convert an image:

```sh
pngquant --force --output icon0.png 256 my_image.png   # resize it to the size above first
```

`template.xml` is optional: with `bg.png` or `startup.png` and no `template.xml`, the
build writes a minimal one that shows them. Add your own only for a custom layout.

Adding or removing a file is picked up on the next `build_vita.py` run, whose
summary lists the images used. If the Vita still shows the old icon after
installing the new VPK, delete the application and install it again (the Vita keeps
the icon of an installed title ID).

These files are not committed (`.gitignore`): only this README is. Do not commit a
logo or image taken from the game or its ROMs: it is Sega's.
