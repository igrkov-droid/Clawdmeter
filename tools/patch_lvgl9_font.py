#!/usr/bin/env python3
"""Convert an lv_font_conv 1.5.x output file to the LVGL 9 struct layout.

lv_font_conv still emits LVGL 8 sources: the font structs are wrapped in
version #ifs, lv_font_fmt_txt_dsc_t carries a `.cache` member that LVGL 9
dropped, and lv_font_t is missing three fields LVGL 9 expects. A font built
without these fixes compiles cleanly and then renders nothing at all, which is
a miserable thing to debug — docs/fonts.md describes the edits, this applies
them.

Usage:  tools/patch_lvgl9_font.py firmware/src/font_foo_20.c [...]
Idempotent: a file that is already patched is left alone.
"""
import re
import sys

# How each version condition resolves under LVGL 9. Anything not listed is
# left untouched — notably the FONT_* include guard around the whole file.
CONDITIONS = {
    "LVGL_VERSION_MAJOR == 8": False,
    "LVGL_VERSION_MAJOR >= 8": True,
    "LVGL_VERSION_MAJOR >= 9": True,
    "!(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)": True,
    "LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8": True,
    "LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9": True,
}

# LVGL 9 fields lv_font_conv never emits, inserted after .subpx.
NEW_FIELDS = [
    "    .release_glyph = NULL,",
    "    .kerning = 0,",
    "    .static_bitmap = 0,",
]


def resolve_conditionals(lines):
    """Flatten the version #ifs, keeping the branch true under LVGL 9."""
    out, stack = [], []          # stack entries: True = emitting this branch
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("#if "):
            cond = stripped[4:].strip()
            if cond in CONDITIONS:
                stack.append(CONDITIONS[cond])
                continue
            stack.append(None)   # not ours — keep the directive verbatim
        elif stripped == "#else" and stack and stack[-1] is not None:
            stack[-1] = not stack[-1]
            continue
        elif stripped == "#endif" and stack:
            known = stack.pop()
            if known is not None:
                continue
        if all(s is not False for s in stack):
            out.append(line)
    return out


def patch(path):
    src = open(path).read()
    if ".static_bitmap" in src and "LVGL_VERSION_MAJOR" not in src:
        print(f"  {path}: уже пропатчен, пропускаю")
        return False

    lines = resolve_conditionals(src.splitlines())

    # The cache object only existed to be assigned to the field we just
    # removed; leaving it would be an unused-variable warning.
    text = "\n".join(lines)
    text = re.sub(r"/\*Store all the custom data of the font\*/\n"
                  r"static\s+lv_font_fmt_txt_glyph_cache_t cache;\n", "", text)

    # Add the fields LVGL 9 reads but lv_font_conv never writes.
    if ".release_glyph" not in text:
        text = text.replace("    .subpx = LV_FONT_SUBPX_NONE,",
                            "    .subpx = LV_FONT_SUBPX_NONE,\n" + "\n".join(NEW_FIELDS), 1)

    open(path, "w").write(text)
    print(f"  {path}: пропатчен под LVGL 9")
    return True


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        patch(p)
