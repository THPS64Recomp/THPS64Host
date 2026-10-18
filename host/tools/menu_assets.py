#!/usr/bin/env python3
"""Stage the in-game settings menu's run-time assets (THPS_MENU builds): documents and styles from ui/menu, the OFL fonts, their licence.

    python3 -I tools/menu_assets.py DEST FONTS_DIR

DEST becomes <resources>/ui (next to the executable, or Contents/Resources/ui in a macOS app). FONTS_DIR is what
tools/setup_runtime.py fetched (<runtime>/ui-fonts). The menu code (src/ui/ui_host.cpp) loads exactly these names.
Used by the CMake build (a bare build, so the tests find them), tools/package_split.py (release package and dev resources).
"""
import os
import shutil
import sys

SHARED = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))     # decomp/common/host
FONTS = ["LatoLatin-Regular.ttf", "LatoLatin-Italic.ttf", "LatoLatin-Bold.ttf", "LatoLatin-BoldItalic.ttf",
         "NotoEmoji-Regular.ttf", os.path.join("promptfont", "promptfont.ttf")]


def stage(dest, fonts_dir):
    """Copy the menu into dest (replaced). Returns an error string, or "" on success."""
    for f in FONTS:
        if not os.path.isfile(os.path.join(fonts_dir, f)):
            return f"{os.path.join(fonts_dir, f)} is missing: run tools/setup_runtime.py <runtime dir> --ui-only"
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(os.path.join(SHARED, "ui", "menu"), dest)
    os.makedirs(os.path.join(dest, "fonts"), exist_ok=True)
    for f in FONTS:
        shutil.copy2(os.path.join(fonts_dir, f), os.path.join(dest, "fonts", os.path.basename(f)))
    shutil.copy2(os.path.join(SHARED, "ui", "FONTS-LICENSE.txt"), os.path.join(dest, "fonts", "LICENSE.txt"))
    return ""


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    err = stage(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]))
    if err:
        sys.exit(err)
    print(f"menu assets: {sys.argv[1]}")
