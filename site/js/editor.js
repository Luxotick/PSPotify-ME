import { startPSP, bindKeys, bindPad } from './psp.js';
import * as T from './theme.js';
import { check, extremes } from './contrast.js';
import { zip, save } from './zip.js';
import { loadIndex, themeUrl } from './themes.js';

const $ = (id) => document.getElementById(id);
const DRAFT = 'pspotify-theme-draft';

let theme = T.defaults();
let wall = null; // { bytes: Uint8Array, ext: 'jpg' | 'png', url, backdrops }
let psp = null;

// ------------------------------------------------------------------ colors
// One row per color: a picker, its hex code and, for detail colors, whether
// it follows its main color.
const rows = {};

function colorRow(key, parent, detail) {
  const row = document.createElement('div');
  row.className = 'color';
  row.innerHTML = `
    <input type="color" aria-label="${T.LABELS[key]}">
    <div class="color-text">
      <label>${T.LABELS[key]}</label>
      <span class="muted small">${T.HINTS[key]}</span>
    </div>
    <input class="hex" spellcheck="false" maxlength="7" aria-label="${T.LABELS[key]} hex">
    ${detail ? `<label class="check small follow"><input type="checkbox"> same as ${T.LABELS[T.DETAIL[key]].toLowerCase()}</label>` : ''}`;
  const [picker, hex] = row.querySelectorAll('input:not([type=checkbox])');
  const follow = row.querySelector('.follow input');
  picker.addEventListener('input', () => { theme[key] = picker.value.toUpperCase(); changed(); });
  hex.addEventListener('input', () => {
    const c = T.parseColor(hex.value.trim());
    hex.classList.toggle('invalid', !c);
    if (c) { theme[key] = c; changed(false); }
  });
  hex.addEventListener('blur', () => fill());
  if (follow) follow.addEventListener('change', () => {
    theme[key] = follow.checked ? undefined : T.resolved(theme, key);
    if (!theme[key]) delete theme[key];
    changed();
  });
  parent.append(row);
  rows[key] = { row, picker, hex, follow };
}

for (const k of T.MAIN) colorRow(k, $('main-colors'), false);
for (const k of Object.keys(T.DETAIL)) colorRow(k, $('detail-colors'), true);

// The fields from `theme`; skipHex keeps the hex box the user is typing in.
function fill(skipHex = false) {
  if (document.activeElement !== $('name')) $('name').value = theme.name;
  for (const [k, r] of Object.entries(rows)) {
    const v = T.resolved(theme, k);
    r.picker.value = v.toLowerCase();
    if (!skipHex || document.activeElement !== r.hex) {
      r.hex.value = v;
      r.hex.classList.remove('invalid');
    }
    if (r.follow) {
      r.follow.checked = !theme[k];
      r.row.classList.toggle('following', !theme[k]);
    }
  }
  $('dim').value = theme.wallpaper_dim;
  $('dim-out').textContent = theme.wallpaper_dim + '%';
}

// ------------------------------------------------------------------ updates
function changed(refill = true) {
  if (refill) fill(); else fill(true);
  const ini = T.write(theme);
  $('ini').textContent = ini;
  if (psp) psp.setTheme(ini);
  const folder = T.folderName(theme.name);
  $('folder-note').textContent = `Folder on the Memory Stick: THEMES/${folder}`;
  $('install-folder').textContent = folder;
  $('install-wall').hidden = !wall;
  warn();
  try {
    localStorage.setItem(DRAFT, ini);
  } catch { /* private mode: no draft */ }
}

function warn() {
  const list = check(theme, wall ? wall.backdrops : null);
  const box = $('warnings');
  box.innerHTML = '';
  if (!list.length) {
    box.innerHTML = '<p class="ok">✓ Every text and icon color reads well on its background.</p>';
    return;
  }
  for (const w of list) {
    const p = document.createElement('p');
    p.className = w.bad ? 'bad' : 'warn';
    p.innerHTML = `<b>${w.bad ? 'Hard to read' : 'Low contrast'}:</b> ${w.what} · ` +
      `${w.ratio.toFixed(1)}:1 <span class="muted">(aim for ${w.min}:1)</span>`;
    box.append(p);
  }
}

$('name').addEventListener('input', () => {
  theme.name = T.cleanName($('name').value) || 'Untitled';
  changed(false);
});
$('dim').addEventListener('input', () => {
  theme.wallpaper_dim = Number($('dim').value);
  changed();
});

// ------------------------------------------------------------------ wallpaper
// Pictures that are not already a 480x272 JPEG or PNG become one: cropped
// to the screen's shape from the center, like the PSP does, and small
// enough for every model (the PSP-1000 refuses pictures over 960 pixels).
async function setWallpaper(blob, typeHint) {
  const url = URL.createObjectURL(blob);
  const img = new Image();
  img.src = url;
  try {
    await img.decode();
  } catch {
    URL.revokeObjectURL(url);
    alert('That picture could not be read. Try a JPEG or PNG.');
    return;
  }
  const canvas = document.createElement('canvas');
  canvas.width = 480;
  canvas.height = 272;
  const g = canvas.getContext('2d');
  let { naturalWidth: w, naturalHeight: h } = img;
  let sx = 0, sy = 0, sw = w, sh = h;
  if (w * 272 > h * 480) { sw = Math.floor(h * 480 / 272); sx = Math.floor((w - sw) / 2); }
  else { sh = Math.floor(w * 272 / 480); sy = Math.floor((h - sh) / 2); }
  g.imageSmoothingQuality = 'high';
  g.drawImage(img, sx, sy, sw, sh, 0, 0, 480, 272);
  const type = typeHint || blob.type;
  let bytes, ext, note;
  if (w === 480 && h === 272 && (type === 'image/jpeg' || type === 'image/png')) {
    bytes = new Uint8Array(await blob.arrayBuffer());
    ext = type === 'image/png' ? 'png' : 'jpg';
    note = `${ext.toUpperCase()}, 480×272, ${Math.ceil(bytes.length / 1024)} KB. Kept as it is.`;
  } else {
    const out = await new Promise((done) => canvas.toBlob(done, 'image/jpeg', 0.9));
    bytes = new Uint8Array(await out.arrayBuffer());
    ext = 'jpg';
    note = `${w}×${h} → cropped and scaled to a 480×272 JPEG, ${Math.ceil(bytes.length / 1024)} KB.`;
  }
  URL.revokeObjectURL(url);
  if (wall) URL.revokeObjectURL(wall.url);
  wall = {
    bytes, ext,
    url: URL.createObjectURL(new Blob([bytes], { type: ext === 'png' ? 'image/png' : 'image/jpeg' })),
    backdrops: extremes(g.getImageData(0, 0, 480, 272)),
  };
  theme.wallpaper = 'wallpaper.' + ext;
  $('wall-thumb').innerHTML = `<img src="${wall.url}" alt="Wallpaper">`;
  $('wall-note').textContent = note;
  $('wall-remove').hidden = false;
  if (psp) psp.setWallpaper(bytes);
  changed();
}

function removeWallpaper() {
  if (wall) URL.revokeObjectURL(wall.url);
  wall = null;
  theme.wallpaper = '';
  $('wall-thumb').innerHTML = '<span>None</span>';
  $('wall-note').textContent = 'JPEG or PNG; it stays in your browser. Best at 480×272.';
  $('wall-remove').hidden = true;
  if (psp) psp.setWallpaper(null);
  changed();
}

$('wall-file').addEventListener('change', (e) => {
  const f = e.target.files[0];
  if (f) setWallpaper(f);
  e.target.value = '';
});
$('wall-remove').addEventListener('click', removeWallpaper);

// ------------------------------------------------------------------ themes
async function loadTheme(text, folder) {
  const { theme: t } = T.parse(text);
  const file = t.wallpaper;
  theme = t;
  if (file && folder) {
    try {
      const res = await fetch(themeUrl(folder, file));
      if (!res.ok) throw new Error(res.status);
      const blob = await res.blob();
      await setWallpaper(blob, file.toLowerCase().endsWith('.png') ? 'image/png' : 'image/jpeg');
      return;
    } catch {
      /* fall through: no wallpaper */
    }
  }
  if (wall) removeWallpaper(); else { theme.wallpaper = ''; changed(); }
}

async function startFrom(folder) {
  const res = await fetch(themeUrl(folder, 'theme.ini'));
  if (res.ok) await loadTheme(await res.text(), folder);
}

const index = await loadIndex().catch(() => []);
const select = $('start-from');
select.replaceChildren(new Option('Start from…', ''),
  ...index.map((t) => new Option(t.parsed.name, encodeURIComponent(t.folder))));
select.addEventListener('change', () => {
  if (select.value) startFrom(decodeURIComponent(select.value));
  select.value = '';
});

// ?theme=Folder opens a gallery theme; otherwise the last draft, or Midnight.
const wanted = new URLSearchParams(location.search).get('theme');
let draft = null;
try { draft = localStorage.getItem(DRAFT); } catch { /* none */ }
if (wanted && index.some((t) => t.folder === wanted)) await startFrom(wanted);
else if (draft) await loadTheme(draft, null);
else if (index.some((t) => t.folder === 'Midnight')) await startFrom('Midnight');
else changed();

// ------------------------------------------------------------------ download
$('download').addEventListener('click', () => {
  const folder = T.folderName(theme.name);
  const files = [{ path: `THEMES/${folder}/theme.ini`, data: T.write(theme).replace(/\n/g, '\r\n') }];
  if (wall) files.push({ path: `THEMES/${folder}/${theme.wallpaper}`, data: wall.bytes });
  files.push({
    path: 'HOW TO INSTALL.txt',
    data: [
      `PSPotify ME theme: ${theme.name}`,
      '',
      'Copy the THEMES folder to PSP/GAME/PSPOTIFY/ on the Memory Stick,',
      'next to EBOOT.PBP. You end up with:',
      '',
      `  PSP/GAME/PSPOTIFY/THEMES/${folder}/theme.ini`,
      '',
      'Then pick the theme under Settings > Themes.',
      wall ? 'For the wallpaper, set Settings > Appearance > Background to "Theme wallpaper".' : '',
      '',
    ].join('\r\n'),
  });
  save(zip(files), `${folder.replace(/ /g, '-')}.zip`);
});

$('copy').addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(T.write(theme));
    $('copy').textContent = 'Copied';
    setTimeout(() => { $('copy').textContent = 'Copy theme.ini'; }, 1500);
  } catch { /* the text is selectable below anyway */ }
});

// ------------------------------------------------------------------ preview
const screen = $('screen');
screen.addEventListener('pointerdown', () => screen.focus());

startPSP($('canvas')).then((p) => {
  psp = p;
  bindKeys(screen, psp);
  bindPad($('pad'), psp);
  psp.setCoverColors($('cover-colors').checked);
  psp.setTheme(T.write(theme));
  if (wall) psp.setWallpaper(wall.bytes);
  $('screen-msg').remove();
  $('cover-colors').addEventListener('change', () => psp.setCoverColors($('cover-colors').checked));
  import('./view3d.js').then((m) => m.offer($('canvas'), psp)).catch(() => {});
}).catch((err) => {
  console.error(err);
  $('screen-msg').textContent = 'The preview could not start in this browser.';
});
