'use strict';

const grid = document.getElementById('grid');
const shareBtn = document.getElementById('share');
const audioBox = document.getElementById('audio');
let sources = [];
let type = 'screen';
let selected = null;

document.getElementById('audioRow').hidden = !window.picker.audioAvailable;
document.getElementById('macAudio').hidden = window.picker.platform !== 'darwin';

function render() {
  const list = sources.filter((s) => s.type === type);
  if (!list.length) {
    const p = document.createElement('p');
    p.className = 'empty';
    p.textContent = type === 'screen' ? 'Nem található képernyő.' : 'Nincs megosztható ablak.';
    grid.replaceChildren(p);
    return;
  }
  grid.replaceChildren(...list.map((s) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'source';
    b.setAttribute('aria-pressed', String(s.id === selected));
    const thumb = document.createElement('div');
    thumb.className = 'thumb';
    if (s.thumbnail) thumb.style.backgroundImage = `url("${s.thumbnail}")`;
    const name = document.createElement('div');
    name.className = 'source-name';
    if (s.icon) {
      const img = document.createElement('img');
      img.src = s.icon;
      img.alt = '';
      name.append(img);
    }
    name.append(document.createTextNode(s.name || 'Képernyő'));
    b.append(thumb, name);
    b.addEventListener('click', () => select(s.id));
    b.addEventListener('dblclick', () => { select(s.id); share(); });
    return b;
  }));
}

function select(id) {
  selected = id;
  shareBtn.disabled = !selected;
  render();
}

function share() {
  if (selected) window.picker.choose(selected, window.picker.audioAvailable && audioBox.checked);
}

async function load() {
  sources = await window.picker.sources();
  if (!sources.some((s) => s.id === selected)) selected = null;
  if (!selected) {
    const first = sources.find((s) => s.type === 'screen');
    if (type === 'screen' && first) selected = first.id;
  }
  shareBtn.disabled = !selected;
  render();
}

for (const tab of document.querySelectorAll('.tab')) {
  tab.addEventListener('click', () => {
    type = tab.dataset.type;
    for (const t of document.querySelectorAll('.tab')) t.setAttribute('aria-selected', String(t === tab));
    render();
  });
}
shareBtn.addEventListener('click', share);
document.getElementById('cancel').addEventListener('click', () => window.picker.cancel());
document.getElementById('refresh').addEventListener('click', load);
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') window.picker.cancel();
  if (e.key === 'Enter') share();
});
load();
