# Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts.

```bash
npm install -g lv_font_conv
```

Generate each one (one at a time — `lv_font_conv` doesn't like loop-driven
invocations) with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (titles, 56px)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 56 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_56.c --lv-include "lvgl.h"

# Styrene B (large numbers 48, panel labels 28, small text 24, minimal 20)
for size in 48 28 24 20; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_styrene_${size}.c --lv-include "lvgl.h"
done

# DejaVu Sans Mono (32px, with spinner Unicode chars)
lv_font_conv --font assets/DejaVuSansMono.ttf \
  -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
  --size 32 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_mono_32.c --lv-include "lvgl.h"
```

**Important:** `lv_font_conv` v1.5.3 outputs LVGL 8 format. Each generated
file must be patched for LVGL 9 compatibility:

1. Remove `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct
2. Remove the `.cache` field from `font_dsc`
3. Add `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0` to the font struct
4. Add `.fallback = NULL`, `.user_data = NULL` to the font struct

Without these patches, fonts compile but render as invisible.

## Cyrillic

Tiempos and Styrene have no Cyrillic glyphs, so text coming from the user —
calendar event titles — draws as blank space. PT Serif and PT Sans fill the gap
through LVGL's per-font fallback: Latin keeps the brand faces, only the missing
code points come from PT.

```bash
lv_font_conv --font assets/PTSerif-Regular.ttf -r 0x400-0x45F \
  --size 34 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_cyr_serif_34.c --lv-include "lvgl.h"
# likewise --size 56 -> font_cyr_serif_56.c
# and PTSans-Regular.ttf --size 20 -> font_cyr_sans_20.c
```

The fallback is attached in `compute_layout()` (`ui.cpp`) via `with_fallback()`,
which copies the font struct and sets `.fallback` on the copy. Do not write the
field into the generated fonts directly: they are `const` and live in read-only
memory, so the store faults at runtime rather than failing to compile.

## Patching for LVGL 9

`tools/patch_lvgl9_font.py <file.c>` applies the four edits listed above
automatically and is idempotent, so it can be re-run after any regeneration:

```bash
python3 tools/patch_lvgl9_font.py firmware/src/font_cyr_sans_20.c
```
