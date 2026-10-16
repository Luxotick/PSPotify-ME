# UI preview on a PC

The new interface lives in `ui/` and does not depend on the PSP. `tools/preview` runs the same code in a 480×272 window with mock data, so you can try layout and theme changes without copying an EBOOT to the console.

## Build

Windows needs Visual Studio with "Desktop development with C++". Linux needs a C compiler and `libsdl2-dev`.

```sh
python scripts/build_preview.py
```

The script downloads pinned copies of SDL2 (Windows only), the M PLUS Rounded 1c fonts and the Lucide icon font into `.deps/`. The program ends up in `build-preview/`.

## Run

```sh
build-preview/PSPotifyPreview
```

| Key | PSP button |
|---|---|
| Arrow keys | D-pad |
| Z or Enter | ✕ |
| X or Backspace | ○ |
| A | □ |
| S | △ |
| Q / E | L / R (hold to seek) |
| Esc | START |

Options:

- `--theme Daylight`: start with `themes/Daylight/theme.ini`.
- `--play`: start with a song playing.
- `--no-cover-colors`: start with Cover colors off.
- `--scale 3`: a bigger window.
- `--shot out.png --keys right,down,cross`: press those buttons, then save a 480×272 screenshot and exit.

## Themes

Each folder in `themes/` is a theme with a `theme.ini`:

```ini
name = Midnight
background = #0E0F13
surface = #1B1D24
text = #F2F2F2
text_dim = #9EA3AE
accent = #1ED760
```

The preview reloads the active theme as soon as you save the file. Pick another theme under Settings. A missing or malformed line keeps the default color.

**Cover colors** (Settings, on by default) tints the theme with the dominant color of the playing track's album art. A dark theme stays dark and a light one light; only the hue changes, so text stays readable. The preview's covers are generated stand-ins; on the PSP it will use the real album art.

## In the browser

`python scripts/build_preview.py --web` builds the preview with Emscripten (emsdk on PATH) into `build-web/`. `--library DIR` replaces the mock tracks and playlists with `DIR/library.h` (and `DIR/covers/`, if there is one); it works for the PC build too.

## Theme site

`site/` is the theme editor and gallery published on GitHub Pages by `.github/workflows/site.yml`. To build and try it locally:

```sh
python scripts/build_preview.py --web --library site/library
python scripts/build_preview.py --library site/library
python scripts/build_site.py
python -m http.server 8000 --directory _site
```

`site/library/library.h` is a made-up library (songs, artists, playlists; the covers are the generated stand-ins), so the public site shows no real music or album art. `build_site.py` copies every `themes/` folder, takes a 480×272 picture of each with the PC preview (`--no-shots` skips that) and writes `themes/index.json` for the gallery and the editor.

The editor sends its `theme.ini` text and wallpaper to the WebAssembly build (`web_set_theme`, `web_set_wallpaper` in `tools/preview/main.c`), which parses and scales them with the same code as the PSP. The PC preview shows a theme's wallpaper as if Background were set to *Theme wallpaper*.
