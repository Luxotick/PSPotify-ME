"""Builds the PC preview of the new UI (tools/preview) with pinned SDL2 and fonts.

Windows: MSVC (found with vswhere). Linux: cc + the system SDL2 (libsdl2-dev).
The preview shows the same ui/ code the PSP runs, with mock data, so the UI
can be iterated on without copying an EBOOT to the console every time.
"""
from pathlib import Path
import argparse
import io
import os
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import ROOT, fetch_sources  # noqa: E402
from ui_assets import FACES, download, fetch as fetch_fonts, subset_face  # noqa: E402

DEPS = ROOT / ".deps"
OUT = ROOT / "build-preview"

SDL2_VC = ("https://github.com/libsdl-org/SDL/releases/download/release-2.32.8/"
           "SDL2-devel-2.32.8-VC.zip",
           "a16e5f0e5de87a804e3c240a7d92aca0318fda176b22d95550c6512fc4f18172")


def fetch_sdl2_windows():
    sdl = DEPS / "SDL2-2.32.8"
    if not sdl.exists():
        print("fetching SDL2 2.32.8 (VC)")
        with zipfile.ZipFile(io.BytesIO(download(*SDL2_VC))) as archive:
            archive.extractall(DEPS)
    return sdl


def vcvars():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    path = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                           "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                           "-property", "installationPath"],
                          check=True, capture_output=True, text=True).stdout.strip()
    if not path:
        raise RuntimeError("MSVC (Desktop development with C++) not found")
    return Path(path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"


SOURCES = ["tools/preview/main.c", "tools/preview/render_sdl.c",
           "ui/xmb.c", "ui/font.c", "ui/theme.c", "ui/draw.c", "ui/palette.c",
           "ui/stb_truetype_impl.c"]


def library_header(library):
    """The library.h of a --library folder, as a forward-slash path."""
    return (Path(library).resolve() / "library.h").as_posix()


def build_windows(library=None):
    sdl = fetch_sdl2_windows()
    OUT.mkdir(exist_ok=True)
    obj = OUT / "obj"
    obj.mkdir(exist_ok=True)
    cl = ["cl", "/nologo", "/O2", "/W3", "/std:c11", "/utf-8", "/D_CRT_SECURE_NO_WARNINGS",
          f"/I{sdl / 'include'}", f"/I{DEPS / 'stb'}", f"/I{ROOT / 'ui'}",
          *([f'/DPREVIEW_LIBRARY="{library_header(library)}"'] if library else []),
          f"/Fo{obj}\\", f"/Fe{OUT / 'PSPotifyPreview.exe'}",
          *[str(ROOT / s) for s in SOURCES],
          "/link", f"/LIBPATH:{sdl / 'lib' / 'x64'}", "SDL2.lib", "shell32.lib",
          "/SUBSYSTEM:CONSOLE"]
    command = f'call "{vcvars()}" >nul && ' + subprocess.list2cmdline(cl)
    subprocess.run(command, shell=True, check=True, cwd=ROOT)
    shutil.copy2(sdl / "lib" / "x64" / "SDL2.dll", OUT / "SDL2.dll")


def build_linux(library=None):
    OUT.mkdir(exist_ok=True)
    flags = subprocess.run(["sdl2-config", "--cflags", "--libs"], check=True,
                           capture_output=True, text=True).stdout.split()
    subprocess.run(["cc", "-O2", "-std=c11", "-Wall", "-D_DEFAULT_SOURCE",
                    f"-I{DEPS / 'stb'}", f"-I{ROOT / 'ui'}",
                    *([f'-DPREVIEW_LIBRARY="{library_header(library)}"'] if library else []),
                    *[str(ROOT / s) for s in SOURCES],
                    "-o", str(OUT / "PSPotifyPreview"), *flags, "-lm"],
                   check=True, cwd=ROOT)


def build_web(library=None):
    """WebAssembly build for a page: emcc (Emscripten) on PATH, SDL2 from its ports.

    The page sends buttons with Module._web_set_buttons(bits) and reads the
    480x272 canvas it passes as Module.canvas. The theme editor sends a
    theme.ini text (web_set_theme) and wallpaper bytes (web_set_wallpaper).
    library: a folder with library.h (and optionally covers/) that replaces
    the mock tracks and playlists."""
    out = ROOT / "build-web"
    out.mkdir(exist_ok=True)
    emcc = shutil.which("emcc") or shutil.which("emcc.bat")
    if not emcc:
        raise RuntimeError("emcc not found (install and activate emsdk)")
    # Subset fonts, like the PSP build: the full M PLUS faces are 3.5 MB each.
    fonts = out / "fonts"
    fonts.mkdir(exist_ok=True)
    for symbol, name in FACES:
        (fonts / name).write_bytes(subset_face(symbol, name))
    cmd = [emcc, "-O2", "-std=c11", "-sUSE_SDL=2", "-sALLOW_MEMORY_GROWTH=1",
           "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=PSPotify",
           "-sENVIRONMENT=web", "-sEXIT_RUNTIME=0",
           "-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_web_set_buttons,_web_preview,_web_play_id,"
           "_web_pos_ms,_web_set_pos_ms,_web_paused,_web_set_theme,_web_set_wallpaper,"
           "_web_set_cover_colors",
           "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,ccall,HEAPU8",
           f"-I{DEPS / 'stb'}", f"-I{ROOT / 'ui'}",
           *[str(ROOT / s) for s in SOURCES],
           "--preload-file", f"{ROOT / 'themes'}@/themes",
           *[a for name in ("MPLUSRounded1c-Regular.ttf", "MPLUSRounded1c-Bold.ttf",
                            "MPLUSRounded1c-ExtraBold.ttf", "lucide.ttf")
             for a in ("--preload-file", f"{fonts / name}@/.deps/fonts/{name}")],
           "-o", "build-web/pspotify.mjs"]  # relative: the path ends up in the .mjs
    if library:
        library = Path(library).resolve()
        cmd[1:1] = [f'-DPREVIEW_LIBRARY="{library_header(library)}"']
        if (library / "covers").is_dir():
            cmd[1:1] = ["--preload-file", f"{library / 'covers'}@/covers"]
    subprocess.run(cmd, check=True, cwd=ROOT)
    print(f"built {out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch-only", action="store_true")
    parser.add_argument("--web", action="store_true", help="build for the browser with Emscripten")
    parser.add_argument("--library", metavar="DIR",
                        help="tracks and playlists from DIR/library.h (and DIR/covers) instead of the mock ones")
    args = parser.parse_args()
    fetch_sources()
    fetch_fonts()
    if args.fetch_only:
        return
    if args.web:
        build_web(args.library)
        return
    if sys.platform == "win32":
        build_windows(args.library)
    else:
        build_linux(args.library)
    print(f"built {OUT}")


if __name__ == "__main__":
    main()
