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
from ui_assets import download, fetch as fetch_fonts  # noqa: E402

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


def build_windows():
    sdl = fetch_sdl2_windows()
    OUT.mkdir(exist_ok=True)
    obj = OUT / "obj"
    obj.mkdir(exist_ok=True)
    cl = ["cl", "/nologo", "/O2", "/W3", "/std:c11", "/utf-8", "/D_CRT_SECURE_NO_WARNINGS",
          f"/I{sdl / 'include'}", f"/I{DEPS / 'stb'}", f"/I{ROOT / 'ui'}",
          f"/Fo{obj}\\", f"/Fe{OUT / 'PSPotifyPreview.exe'}",
          *[str(ROOT / s) for s in SOURCES],
          "/link", f"/LIBPATH:{sdl / 'lib' / 'x64'}", "SDL2.lib", "shell32.lib",
          "/SUBSYSTEM:CONSOLE"]
    command = f'call "{vcvars()}" >nul && ' + subprocess.list2cmdline(cl)
    subprocess.run(command, shell=True, check=True, cwd=ROOT)
    shutil.copy2(sdl / "lib" / "x64" / "SDL2.dll", OUT / "SDL2.dll")


def build_linux():
    OUT.mkdir(exist_ok=True)
    flags = subprocess.run(["sdl2-config", "--cflags", "--libs"], check=True,
                           capture_output=True, text=True).stdout.split()
    subprocess.run(["cc", "-O2", "-std=c11", "-Wall", "-D_DEFAULT_SOURCE",
                    f"-I{DEPS / 'stb'}", f"-I{ROOT / 'ui'}",
                    *[str(ROOT / s) for s in SOURCES],
                    "-o", str(OUT / "PSPotifyPreview"), *flags, "-lm"],
                   check=True, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch-only", action="store_true")
    args = parser.parse_args()
    fetch_sources()
    fetch_fonts()
    if args.fetch_only:
        return
    if sys.platform == "win32":
        build_windows()
    else:
        build_linux()
    print(f"built {OUT}")


if __name__ == "__main__":
    main()
