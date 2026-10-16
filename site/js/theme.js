// theme.ini in JavaScript: reads it with the rules of ui/theme.c
// (theme_parse) to fill the editor, and writes it back. The preview itself
// always parses the text with the C code, so what it shows is what the PSP shows.

export const MAIN = ['background', 'surface', 'text', 'text_dim', 'accent'];
// Optional colors and the main color each one follows when it is not set.
export const DETAIL = { highlight: 'text', progress: 'accent', player_bar: 'surface', icons: 'text' };

export const LABELS = {
  background: 'Background', surface: 'Surface', text: 'Text', text_dim: 'Dim text',
  accent: 'Accent', highlight: 'Selected title', progress: 'Progress bars',
  player_bar: 'Mini player', icons: 'Category icons',
};
export const HINTS = {
  background: 'Behind everything',
  surface: 'Search box, mini player',
  text: 'Titles, clock, icons',
  text_dim: 'Subtitles, button hints, times',
  accent: 'Playing song, play button, toggles',
  highlight: 'Title of the selected row',
  progress: 'Filled part of the progress bars',
  player_bar: 'The bar at the bottom',
  icons: 'The categories across the top',
};

// theme_defaults() in ui/theme.c.
export function defaults() {
  return {
    name: 'Midnight', background: '#0E0F13', surface: '#1B1D24', text: '#F2F2F2',
    text_dim: '#9EA3AE', accent: '#1ED760', wallpaper: '', wallpaper_dim: 40,
  };
}

// The value of a detail color: its own, or the main color it follows.
export const resolved = (t, key) => t[key] || t[DETAIL[key]] || t[key];

// #RGB or #RRGGBB, the '#' optional; null if malformed. Returns #RRGGBB.
export function parseColor(s) {
  s = s.startsWith('#') ? s.slice(1) : s;
  if (!/^([0-9a-f]{3}|[0-9a-f]{6})$/i.test(s)) return null;
  if (s.length === 3) s = [...s].map((c) => c + c).join('');
  return '#' + s.toUpperCase();
}

const COLOR_KEYS = [...MAIN, ...Object.keys(DETAIL)];

// theme_parse(): the lines of text over the defaults. `ignored` counts lines
// the PSP ignores too.
export function parse(text) {
  const t = defaults();
  let ignored = 0;
  for (let line of text.split('\n')) {
    line = line.trim();
    if (!line || '#;['.includes(line[0])) continue;
    const eq = line.indexOf('=');
    if (eq < 0) { ignored++; continue; }
    const key = line.slice(0, eq).trim();
    let value = line.slice(eq + 1).trim();
    // An inline comment: '#' or ';' after a space.
    const m = /\s[#;]/.exec(value);
    if (m) value = value.slice(0, m.index).trim();
    if (key === 'name') { t.name = cut(value, 31); continue; }
    if (key === 'wallpaper') {
      if (!value || utf8(value) >= 48 || /[/\\:]/.test(value)) ignored++;
      else t.wallpaper = value;
      continue;
    }
    if (key === 'wallpaper_dim') {
      const v = /^[+-]?\d+$/.test(value) ? Number(value) : NaN;
      if (!(v >= 0 && v <= 100)) ignored++; else t.wallpaper_dim = v;
      continue;
    }
    if (!COLOR_KEYS.includes(key)) { ignored++; continue; }
    const c = parseColor(value);
    if (c) t[key] = c; else ignored++;
  }
  return { theme: t, ignored };
}

const utf8 = (s) => new TextEncoder().encode(s).length;

// The first `max` bytes of s in UTF-8, without cutting a character in half.
function cut(s, max) {
  let out = '';
  for (const ch of s) {
    if (utf8(out + ch) > max) break;
    out += ch;
  }
  return out;
}

// A name the parser reads back unchanged: one line, no comment characters.
export const cleanName = (s) => cut(s.replace(/[\r\n#;]/g, '').trim(), 31);

// The theme's folder on the Memory Stick: plain ASCII, like the stock themes.
export function folderName(name) {
  const f = name.normalize('NFKD').replace(/[̀-ͯ]/g, '').replace(/ı/g, 'i')
    .replace(/[^A-Za-z0-9 _-]/g, '').replace(/\s+/g, ' ').trim().slice(0, 31).trim();
  return f || 'My theme';
}

export function write(t) {
  const lines = ['# PSPotify ME theme. Colors are #RRGGBB; a missing line keeps the default.',
    `name = ${cleanName(t.name) || 'Untitled'}`];
  for (const k of MAIN) lines.push(`${k} = ${t[k]}`);
  for (const k of Object.keys(DETAIL)) if (t[k]) lines.push(`${k} = ${t[k]}`);
  if (t.wallpaper) {
    lines.push(`wallpaper = ${t.wallpaper}`);
    lines.push(`wallpaper_dim = ${t.wallpaper_dim}`);
  }
  return lines.join('\n') + '\n';
}
