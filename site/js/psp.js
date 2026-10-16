// Runs the PSPotify ME interface (tools/preview built with
// `python scripts/build_preview.py --web --library site/library`) on a
// 480x272 canvas. The page owns the input and tells the program which PSP
// buttons are held; the theme editor sends theme.ini text and wallpapers.

// UI_BTN_* in ui/ui.h.
export const BTN = {
  up: 1 << 0, down: 1 << 1, left: 1 << 2, right: 1 << 3,
  cross: 1 << 4, circle: 1 << 5, square: 1 << 6, triangle: 1 << 7,
  l: 1 << 8, r: 1 << 9, start: 1 << 10, select: 1 << 11,
};

// The same keys as the desktop preview (docs/PREVIEW.md).
export const KEYS = {
  ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right',
  KeyZ: 'cross', Enter: 'cross', KeyX: 'circle', Backspace: 'circle',
  KeyA: 'square', KeyS: 'triangle', KeyQ: 'l', KeyE: 'r', Tab: 'select', Escape: 'start',
};

// The program reads the buttons once per frame, so a press shorter than a
// frame would be lost: every press stays down at least this long (ms) and
// at least two frames (a hidden or busy tab may draw only a few per second).
const MIN_HOLD = 60;
let frames = 0;
const countFrames = () => { frames++; requestAnimationFrame(countFrames); };
requestAnimationFrame(countFrames);

export async function startPSP(canvas) {
  const url = new URL('../wasm/pspotify.mjs', import.meta.url).href;
  const { default: PSPotify } = await import(url);
  const title = document.title; // SDL renames the page after its window
  const module = await PSPotify({
    canvas,
    arguments: ['--play', '--no-cover-colors'],
    // .wasm and .data sit next to the .mjs, not next to the page.
    locateFile: (path) => new URL(path, url).href,
    print: () => {},
    printErr: (text) => console.warn('[pspotify]', text),
  });
  document.title = title;

  // Buttons held from every source (keyboard, the on-page pad, the 3D model).
  const sources = new Map();
  const send = () => {
    let bits = 0;
    for (const b of sources.values()) bits |= b;
    module._web_set_buttons(bits >>> 0);
  };
  const downAt = new Map(); // source -> { t, frame } of its latest press

  return {
    // Holds `name` (a BTN key) for the source until release() is called.
    press(source, name) {
      downAt.set(source, { t: performance.now(), frame: frames });
      sources.set(source, (sources.get(source) || 0) | BTN[name]);
      send();
    },
    release(source, name) {
      const since = downAt.get(source);
      const done = () => {
        if (downAt.get(source) !== since) return; // pressed again meanwhile
        if (since && frames < since.frame + 2) return requestAnimationFrame(done);
        const bits = (sources.get(source) || 0) & ~BTN[name];
        if (bits) sources.set(source, bits); else sources.delete(source);
        send();
      };
      setTimeout(done, since ? Math.max(0, MIN_HOLD - (performance.now() - since.t)) : 0);
    },
    releaseAll() { sources.clear(); send(); },
    setTheme(ini) { module.ccall('web_set_theme', null, ['string'], [ini]); },
    // bytes: a Uint8Array with a JPEG or PNG, or null for none.
    setWallpaper(bytes) {
      if (!bytes || !bytes.length) return module._web_set_wallpaper(0, 0);
      const ptr = module._malloc(bytes.length);
      module.HEAPU8.set(bytes, ptr);
      module._web_set_wallpaper(ptr, bytes.length);
      module._free(ptr);
    },
    setCoverColors(on) { module._web_set_cover_colors(on ? 1 : 0); },
  };
}

// Keyboard input for an element: keys only reach the PSP while it has focus,
// so typing in the editor's fields never moves the interface.
export function bindKeys(el, psp) {
  el.addEventListener('keydown', (e) => {
    const b = KEYS[e.code];
    if (!b || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.code === 'Tab' && e.shiftKey) return; // keep a way out of the screen
    e.preventDefault();
    if (!e.repeat) psp.press('key:' + b, b);
  });
  el.addEventListener('keyup', (e) => {
    const b = KEYS[e.code];
    if (b) psp.release('key:' + b, b);
  });
  el.addEventListener('blur', () => psp.releaseAll());
}

// Clickable buttons: every element with data-btn="cross" (and so on).
export function bindPad(root, psp) {
  for (const el of root.querySelectorAll('[data-btn]')) {
    const b = el.dataset.btn, source = 'pad:' + b;
    el.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      el.setPointerCapture(e.pointerId);
      el.classList.add('down');
      psp.press(source, b);
    });
    const up = () => { el.classList.remove('down'); psp.release(source, b); };
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
  }
}
