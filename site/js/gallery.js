import { loadIndex, themeUrl, fetchFiles } from './themes.js';
import { MAIN, resolved } from './theme.js';
import { zip, save } from './zip.js';

const grid = document.getElementById('grid');

function card(t) {
  const el = document.createElement('article');
  el.className = 'card';
  const shot = document.createElement('div');
  shot.className = 'shot';
  shot.style.background = t.parsed.background;
  if (t.preview) {
    const img = new Image(480, 272);
    img.src = themeUrl(t.folder, t.preview);
    img.alt = `${t.parsed.name} on the PSP`;
    img.loading = 'lazy';
    shot.append(img);
  }
  const body = document.createElement('div');
  body.className = 'card-body';
  const title = document.createElement('h3');
  title.textContent = t.parsed.name;
  const swatches = document.createElement('div');
  swatches.className = 'swatches';
  for (const k of MAIN) {
    const s = document.createElement('span');
    s.style.background = resolved(t.parsed, k);
    s.title = `${k} ${resolved(t.parsed, k)}`;
    swatches.append(s);
  }
  if (t.parsed.wallpaper) {
    const w = document.createElement('span');
    w.className = 'tag';
    w.textContent = 'wallpaper';
    swatches.append(w);
  }
  const row = document.createElement('div');
  row.className = 'row';
  const open = document.createElement('a');
  open.className = 'button ghost';
  open.href = `editor.html?theme=${encodeURIComponent(t.folder)}`;
  open.textContent = 'Open in editor';
  const dl = document.createElement('button');
  dl.type = 'button';
  dl.className = 'button primary';
  dl.textContent = 'Download';
  dl.addEventListener('click', async () => {
    dl.disabled = true;
    try {
      save(zip(await fetchFiles(t)), `${t.folder.replace(/ /g, '-')}.zip`);
    } catch (err) {
      console.error(err);
      alert('The download failed. Try again in a moment.');
    }
    dl.disabled = false;
  });
  row.append(open, dl);
  body.append(title, swatches, row);
  el.append(shot, body);
  return el;
}

try {
  const list = await loadIndex();
  grid.replaceChildren(...list.map(card));
  if (!list.length) grid.innerHTML = '<p class="muted">No themes yet.</p>';
} catch (err) {
  console.error(err);
  grid.innerHTML = '<p class="muted">The themes could not be loaded.</p>';
}
