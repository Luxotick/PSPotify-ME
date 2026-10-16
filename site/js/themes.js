// The gallery's themes: themes/index.json, written by scripts/build_site.py
// from the repository's themes/ folder.
import { parse } from './theme.js';

const base = new URL('../themes/', import.meta.url);

export const themeUrl = (folder, file) =>
  new URL(`${encodeURIComponent(folder)}/${encodeURIComponent(file)}`, base).href;

// [{ folder, files: [...], preview: 'preview.png' | null, ini, parsed }]
export async function loadIndex() {
  const res = await fetch(new URL('index.json', base));
  if (!res.ok) throw new Error(`index.json: ${res.status}`);
  const list = await res.json();
  for (const t of list) t.parsed = parse(t.ini).theme;
  return list;
}

export async function fetchFiles(t) {
  return Promise.all(t.files.map(async (file) => {
    const res = await fetch(themeUrl(t.folder, file));
    if (!res.ok) throw new Error(`${file}: ${res.status}`);
    return { path: `THEMES/${t.folder}/${file}`, data: new Uint8Array(await res.arrayBuffer()) };
  }));
}
