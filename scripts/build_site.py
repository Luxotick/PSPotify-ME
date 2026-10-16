"""Builds the theme site (site/) into a folder GitHub Pages can serve.

    python scripts/build_preview.py --web --library site/library
    python scripts/build_preview.py --library site/library
    python scripts/build_site.py

The first build gives the browser preview (build-web/), the second the PC
preview, which takes each theme's gallery picture. The output (_site/ by
default) holds the pages, the WebAssembly preview, and every theme from
themes/ with a preview.png and an index.json the gallery and editor read,
plus psp.txt, the same list for the PSP's theme gallery.

With --release-zip and --release-tag it also publishes that release's EBOOT
and update.txt, which the PSP checks for updates.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent

# Each gallery picture: Playlists with a song in the mini player, the
# theme's own colors (no Cover colors), a row below the top selected.
SHOT_ARGS = ["--play", "--no-cover-colors", "--keys", "down"]

ACCENT = re.compile(r"^[ \t]*accent[ \t]*=[ \t]*#?([0-9A-Fa-f]{6}|[0-9A-Fa-f]{3})\b", re.M)


def psp_line(folder, ini, files):
    """A theme for the PSP's gallery: folder, accent and files, tab-separated."""
    m = ACCENT.search(ini)
    accent = m.group(1) if m else "475569"
    if len(accent) == 3:
        accent = "".join(c * 2 for c in accent)
    return "\t".join([folder, "#" + accent.upper(), *files])


def add_update(out, zip_path, tag):
    """Publishes a release's EBOOT and update.txt for the app's update check."""
    with zipfile.ZipFile(zip_path) as archive:
        eboot = archive.read("PSP/GAME/PSPOTIFY/EBOOT.PBP")
        names = archive.namelist()
        meta = json.loads(archive.read("BUILD.json")) if "BUILD.json" in names else {}
    tag_version = re.sub(r"^v", "", tag).split("-")[0]
    version = meta.get("version") or tag_version
    if version != tag_version:
        # The app would offer this update forever; leave the last good one.
        print(f"warning: release {tag} holds version {version}, not published as an update")
        return
    (out / "update").mkdir()
    (out / "update" / "EBOOT.PBP").write_bytes(eboot)
    (out / "update.txt").write_text(
        "# The latest PSPotify ME release, read by the app's update check.\n"
        f"version = {version}\ntag = {tag}\nsize = {len(eboot)}\n"
        f"sha256 = {hashlib.sha256(eboot).hexdigest()}\neboot = update/EBOOT.PBP\n",
        encoding="utf-8")
    print(f"update {version} ({tag}, {len(eboot)} bytes)")


def preview_program():
    folder = ROOT / "build-preview"
    for name in ("PSPotifyPreview.exe", "PSPotifyPreview"):
        if (folder / name).exists():
            return folder / name
    return None


def build(out, shots, release_zip=None, release_tag=None):
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
    psp = ["# PSPotify ME theme gallery for the PSP: folder, accent, files (tab-separated)"]
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
            psp.append(psp_line(folder.name, entry["ini"], files))
            print(f"theme {folder.name}: {len(files)} file(s){' + preview' if program else ''}")
    finally:
        shutil.rmtree(shot_root, ignore_errors=True)
    (themes_out / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=1),
                                           encoding="utf-8")
    (themes_out / "psp.txt").write_text("\n".join(psp) + "\n", encoding="utf-8")
    if release_zip:
        add_update(out, Path(release_zip), release_tag or "")
    (out / ".nojekyll").write_text("")
    print(f"built {out} ({len(index)} themes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=str(ROOT / "_site"), help="output folder (default: _site)")
    parser.add_argument("--no-shots", action="store_true", help="skip the gallery pictures")
    parser.add_argument("--release-zip", help="a release's PSPotify-ME.zip, for update.txt")
    parser.add_argument("--release-tag", help="that release's tag, e.g. v0.3.1-alpha")
    args = parser.parse_args()
    build(Path(args.out).resolve(), not args.no_shots, args.release_zip, args.release_tag)


if __name__ == "__main__":
    main()
