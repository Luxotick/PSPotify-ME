// Readability checks for a theme: WCAG contrast ratios of the color pairs
// the interface actually draws (ui/xmb.c).
import { resolved } from './theme.js';

const rgb = (hex) => [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16));
const hex = (c) => '#' + c.map((v) => Math.round(v).toString(16).padStart(2, '0')).join('').toUpperCase();

function luminance(c) {
  const [r, g, b] = rgb(c).map((v) => {
    v /= 255;
    return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function ratio(a, b) {
  const la = luminance(a), lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

// color_mix(a, b, f) in ui/draw.c: f of b over a.
export const mix = (a, b, f) => hex(rgb(a).map((v, i) => v + (rgb(b)[i] - v) * f));

// fg, bg, minimum ratio, what it is. Body text wants 4.5:1; secondary text,
// icons and large shapes 3:1.
const PAIRS = [
  ['text', 'background', 4.5, 'Titles on the background'],
  ['text_dim', 'background', 3, 'Subtitles and button hints on the background'],
  ['highlight', 'background', 4.5, 'Selected title on the background'],
  ['accent', 'background', 3, 'Playing song (accent) on the background'],
  ['icons', 'background', 3, 'Category icons on the background'],
  ['text', 'surface', 4.5, 'Search text and menus on the surface'],
  ['text_dim', 'surface', 3, 'Dim text on the surface (search hint)'],
  ['text', 'player_bar', 4.5, 'Song title in the mini player'],
  ['text_dim', 'player_bar', 3, 'Artist and times in the mini player'],
  ['background', 'accent', 3, 'Play icon on the accent button'],
];

// backdrops: for a wallpaper, its darkest and lightest typical colors; the
// background color covers them at wallpaper_dim percent.
export function check(theme, backdrops = null) {
  const out = [];
  for (const [fk, bk, min, what] of PAIRS) {
    const fg = resolved(theme, fk);
    let bgs = [resolved(theme, bk)];
    let where = '';
    if (bk === 'background' && backdrops) {
      bgs = backdrops.map((w) => mix(w, theme.background, theme.wallpaper_dim / 100));
      where = ' (over the wallpaper)';
    }
    const r = Math.min(...bgs.map((b) => ratio(fg, b)));
    if (r < min) out.push({ fg: fk, bg: bk, ratio: r, min, what: what + where, bad: r < min * 0.66 });
  }
  return out.sort((a, b) => a.ratio / a.min - b.ratio / b.min);
}

// The 10th and 90th percentile colors of a 480x272 picture by luminance.
export function extremes(imageData) {
  const px = [];
  const d = imageData.data;
  for (let i = 0; i < d.length; i += 4 * 7) {
    const c = hex([d[i], d[i + 1], d[i + 2]]);
    px.push([luminance(c), c]);
  }
  px.sort((a, b) => a[0] - b[0]);
  return [px[Math.floor(px.length * 0.1)][1], px[Math.floor(px.length * 0.9)][1]];
}
