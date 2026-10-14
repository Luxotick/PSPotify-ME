"""Builds the PC preview of the new UI (tools/preview) with pinned SDL2 and fonts.

Windows: MSVC (found with vswhere). Linux: cc + the system SDL2 (libsdl2-dev).
The preview shows the same ui/ code the PSP runs, with mock data, so the UI
can be iterated on without copying an EBOOT to the console every time.
"""
from pathlib import Path
import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import ROOT, fetch_sources  # noqa: E402

DEPS = ROOT / ".deps"
FONTS = DEPS / "fonts"
OUT = ROOT / "build-preview"

GOOGLE_FONTS = ("https://raw.githubusercontent.com/google/fonts/"
                "84efd8ad78c3710ad14bd909e3bc407151885628/ofl/mplusrounded1c/")
DOWNLOADS = {
    "MPLUSRounded1c-Regular.ttf": (GOOGLE_FONTS + "MPLUSRounded1c-Regular.ttf",
        "b75708b53e45b06d17d470aeeca5b766e3d1b3999f03f13ec4eb863ca846c14c"),
    "MPLUSRounded1c-Bold.ttf": (GOOGLE_FONTS + "MPLUSRounded1c-Bold.ttf",
        "c358630584e8e2d8fbd6121d0f4693255ffef6d1e6d4f3441fd6e5a963a11f9e"),
    "MPLUSRounded1c-ExtraBold.ttf": (GOOGLE_FONTS + "MPLUSRounded1c-ExtraBold.ttf",
        "8e7c15901dca87f1451b356dda594f7d092ba252a5dcc47da74523a242493c36"),
}
LUCIDE = ("https://registry.npmjs.org/lucide-static/-/lucide-static-1.48.0.tgz",
          "3c2ecda3d25f6a9692d83f8036d9a526f7da584a51af74cd16eda4498c5c33d8")
SDL2_VC = ("https://github.com/libsdl-org/SDL/releases/download/release-2.32.8/"
           "SDL2-devel-2.32.8-VC.zip",
           "a16e5f0e5de87a804e3c240a7d92aca0318fda176b22d95550c6512fc4f18172")


def download(url, sha256):
    with urllib.request.urlopen(url, timeout=120) as response:
        data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != sha256:
        raise RuntimeError(f"{url}: sha256 {digest} does not match the pin {sha256}")
    return data


def fetch_assets():
    FONTS.mkdir(parents=True, exist_ok=True)
    for name, (url, sha256) in DOWNLOADS.items():
        target = FONTS / name
        if not target.exists():
            print(f"fetching {name}")
            target.write_bytes(download(url, sha256))
    icons = FONTS / "lucide.ttf"
    if not icons.exists():
        print("fetching lucide icons")
        with tarfile.open(fileobj=io.BytesIO(download(*LUCIDE)), mode="r:gz") as tar:
            icons.write_bytes(tar.extractfile("package/font/lucide.ttf").read())
            (FONTS / "lucide-LICENSE").write_bytes(tar.extractfile("package/LICENSE").read())


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
           "ui/xmb.c", "ui/font.c", "ui/theme.c", "ui/draw.c", "ui/palette.c"]


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
    fetch_assets()
    if args.fetch_only:
        return
    if sys.platform == "win32":
        build_windows()
    else:
        build_linux()
    print(f"built {OUT}")


if __name__ == "__main__":
    main()
