"""Builds the theme site (site/) into a folder GitHub Pages can serve.

    python scripts/build_preview.py --web --library site/library
    python scripts/build_preview.py --library site/library
    python scripts/build_site.py

The first build gives the browser preview (build-web/), the second the PC
preview, which takes each theme's gallery picture. The output (_site/ by
default) holds the pages, the WebAssembly preview, and every theme from
themes/ with a preview.png and an index.json the gallery and editor read.
"""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

# Each gallery picture: Playlists with a song in the mini player, the
# theme's own colors (no Cover colors), a row below the top selected.
SHOT_ARGS = ["--play", "--no-cover-colors", "--keys", "down"]


def preview_program():
    folder = ROOT / "build-preview"
    for name in ("PSPotifyPreview.exe", "PSPotifyPreview"):
        if (folder / name).exists():
            return folder / name
    return None


def build(out, shots):
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(ROOT / "site", out, ignore=shutil.ignore_patterns("library", "*.md"))

    wasm = out / "wasm"
    wasm.mkdir()
    for name in ("pspotify.mjs", "pspotify.wasm", "pspotify.data"):
        src = ROOT / "build-web" / name
        if not src.exists():
            raise SystemExit(f"{src} is missing: run scripts/build_preview.py --web --library site/library")
        shutil.copy2(src, wasm / name)

    program = preview_program() if shots else None
    if shots and not program:
        raise SystemExit("build-preview/ has no PSPotifyPreview: run scripts/build_preview.py "
                         "--library site/library, or pass --no-shots")
    env = dict(os.environ)
    if sys.platform != "win32":
        env.setdefault("SDL_VIDEODRIVER", "dummy")  # no display on the CI runner
    # The preview lists 16 themes like the PSP, so each picture is taken in a
    # root of its own that holds the fonts and only that theme.
    shot_root = Path(tempfile.mkdtemp(prefix="pspotify-shots-"))
    if program:
        shutil.copytree(ROOT / ".deps" / "fonts", shot_root / ".deps" / "fonts")
    themes_out = out / "themes"
    themes_out.mkdir()
    index = []
    try:
        for folder in sorted(p for p in (ROOT / "themes").iterdir() if (p / "theme.ini").is_file()):
            dest = themes_out / folder.name
            shutil.copytree(folder, dest)
            files = sorted(f.name for f in folder.iterdir() if f.is_file())
            entry = {"folder": folder.name, "files": files, "preview": None,
                     "ini": (folder / "theme.ini").read_text(encoding="utf-8", errors="replace")}
            if program:
                themes = shot_root / "themes"
                shutil.rmtree(themes, ignore_errors=True)
                shutil.copytree(folder, themes / folder.name)
                subprocess.run([str(program), "--root", str(shot_root), "--theme", folder.name,
                                *SHOT_ARGS, "--shot", str(dest / "preview.png")],
                               check=True, env=env, capture_output=True)
                entry["preview"] = "preview.png"
            index.append(entry)
            print(f"theme {folder.name}: {len(files)} file(s){' + preview' if program else ''}")
    finally:
        shutil.rmtree(shot_root, ignore_errors=True)
    (themes_out / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=1),
                                           encoding="utf-8")
    (out / ".nojekyll").write_text("")
    print(f"built {out} ({len(index)} themes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=str(ROOT / "_site"), help="output folder (default: _site)")
    parser.add_argument("--no-shots", action="store_true", help="skip the gallery pictures")
    args = parser.parse_args()
    build(Path(args.out).resolve(), not args.no_shots)


if __name__ == "__main__":
    main()
