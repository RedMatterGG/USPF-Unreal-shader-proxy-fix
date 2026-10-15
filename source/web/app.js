'use strict';
// USPF front end. Talks to the native host (main.cpp) with chrome.webview messages:
// {id, method, params} -> {id, result} | {id, error}. In a plain browser (development) a mock
// backend from dev/mock.js answers instead.

const api = (() => {
  if (window.chrome && window.chrome.webview) {
    let next = 0;
    const pending = new Map();
    window.chrome.webview.addEventListener('message', (e) => {
      const m = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;
      const p = pending.get(m.id);
      if (!p) return;
      pending.delete(m.id);
      if (m.error) p.reject(new Error(m.error));
      else p.resolve(m.result);
    });
    return (method, params = {}) => new Promise((resolve, reject) => {
      const id = ++next;
      pending.set(id, { resolve, reject });
      window.chrome.webview.postMessage(JSON.stringify({ id, method, params }));
    });
  }
  return (method, params = {}) => window.USPF_MOCK(method, params);
})();

const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const h = (html) => { const t = document.createElement('template'); t.innerHTML = html.trim(); return t.content.firstElementChild; };
const store = {
  get(k, d) { try { const v = localStorage.getItem('uspf.' + k); return v == null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('uspf.' + k, JSON.stringify(v)); } catch { /* storage unavailable */ } },
};

const fmt = {
  n(v, d = 1) { return v == null || Number.isNaN(v) ? '—' : Number(v).toFixed(d); },
  int(v) { return v == null || Number.isNaN(v) ? '—' : Math.round(v).toLocaleString('en-US'); },
  bytes(b) {
    if (!b) return '0 B';
    const u = ['B', 'KB', 'MB', 'GB'];
    let i = 0;
    while (b >= 1024 && i < u.length - 1) { b /= 1024; i++; }
    return `${b.toFixed(i ? 1 : 0)} ${u[i]}`;
  },
  dur(s) {
    if (s == null) return '—';
    s = Math.round(s);
    const m = Math.floor(s / 60);
    return m ? `${m} min ${s % 60} s` : `${s} s`;
  },
  clock(s) { s = Math.max(0, Math.round(s)); const m = Math.floor(s / 60); return `${m}:${String(s % 60).padStart(2, '0')}`; },
  stamp(st) { // 2026-10-10_06-30-21 -> 2026-10-10 06:30
    if (!st) return '';
    const [d, t] = st.split('_');
    return `${d} ${(t || '').slice(0, 5).replace('-', ':')}`;
  },
};

const state = {
  games: [], gameId: store.get('game', null), status: null, version: '', elevated: false,
  runs: [], runId: null, run: null, ini: null, edits: {}, settingsMode: store.get('settingsMode', 'basic'),
  tab: 'dashboard', art: {}, charts: {},
};
const currentGame = () => state.games.find((g) => g.id === state.gameId) || null;

// ---------------------------------------------------------------- toast, modal, tooltip
let toastTimer;
function toast(msg, bad = false) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'toast' + (bad ? ' bad' : '');
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, bad ? 7000 : 3500);
}

function modal({ title, body, actions }) {
  return new Promise((resolve) => {
    $('#modalTitle').textContent = title;
    const b = $('#modalBody');
    b.innerHTML = '';
    if (typeof body === 'string') b.innerHTML = body; else b.append(body);
    const a = $('#modalActions');
    a.innerHTML = '';
    const close = (v) => { $('#modal').hidden = true; document.removeEventListener('keydown', onKey); resolve(v); };
    const onKey = (e) => { if (e.key === 'Escape') close(null); };
    document.addEventListener('keydown', onKey);
    for (const act of actions) {
      const btn = h(`<button class="btn ${act.cls || ''}">${esc(act.label)}</button>`);
      btn.onclick = () => { if (!act.enabled || act.enabled()) close(act.value); };
      if (act.enabled) { btn.disabled = !act.enabled(); act.button = btn; }
      a.append(btn);
    }
    $('#modal').hidden = false;
    const input = $('input', b);
    if (input) {
      input.focus();
      input.addEventListener('input', () => actions.forEach((x) => { if (x.button) x.button.disabled = !x.enabled(); }));
    } else a.lastElementChild?.focus();
  });
}

// Deleting caches needs YES typed, exactly like the old installer.
async function confirmTyped(title, html) {
  const body = h(`<div>${html}<p>Type <b>YES</b> to continue.</p><input class="input" id="typedYes" autocomplete="off" spellcheck="false"></div>`);
  return modal({
    title, body, actions: [
      { label: 'Cancel', value: false, cls: 'ghost' },
      { label: 'Delete', value: true, cls: 'danger', enabled: () => $('#typedYes', body).value === 'YES' },
    ],
  });
}

document.addEventListener('mouseover', (e) => {
  const el = e.target.closest('[data-tip]');
  const tip = $('#tip');
  if (!el) { tip.hidden = true; return; }
  tip.textContent = el.dataset.tip;
  tip.hidden = false;
  const r = el.getBoundingClientRect();
  const w = tip.offsetWidth, hh = tip.offsetHeight;
  let x = r.left + r.width / 2 - w / 2, y = r.bottom + 8;
  if (y + hh > innerHeight - 8) y = r.top - hh - 8;
  tip.style.left = `${Math.max(8, Math.min(x, innerWidth - w - 8))}px`;
  tip.style.top = `${y}px`;
});

async function call(method, params, { quiet = false } = {}) {
  try {
    const r = await api(method, params);
    if (r && r.error && !quiet) toast(r.error, true);
    return r;
  } catch (e) {
    if (!quiet) toast(e.message, true);
    throw e;
  }
}

// ---------------------------------------------------------------- navigation
function showTab(tab) {
  if (state.tab === 'settings' && tab !== 'settings' && Object.keys(state.edits).length) {
    // keep edits; the Save button stays lit when coming back
  }
  state.tab = tab;
  $$('.nav-item').forEach((b) => b.classList.toggle('active', b.dataset.tab === tab));
  $$('.tab').forEach((t) => t.classList.toggle('active', t.id === 'tab-' + tab));
  store.set('tab', tab);
  if (tab === 'performance') loadRuns();
  if (tab === 'settings') loadIni();
  if (tab === 'about') $('#aboutVersion').textContent = `Version ${state.version}`;
  requestAnimationFrame(resizeCharts);
}
$$('.nav-item').forEach((b) => { b.onclick = () => showTab(b.dataset.tab); });

// ---------------------------------------------------------------- game picker
function iconHtml(g) {
  const a = state.art[g.id];
  if (a && a.url) return `<span class="picker-icon" style="background-image:url('${a.url}')"></span>`;
  const initials = g.name.split(/\s+/).filter(Boolean).slice(0, 2).map((w) => w[0]).join('').toUpperCase();
  return `<span class="picker-icon">${esc(initials)}</span>`;
}

function renderPicker() {
  const g = currentGame();
  $('#pickerName').textContent = g ? g.name : (state.games.length ? 'Select a game' : 'No games found');
  $('#pickerSub').textContent = g ? `${g.source}${g.project ? ' · ' + g.project : ''}` : '';
  const icon = $('#pickerIcon');
  icon.outerHTML = g ? iconHtml(g).replace('class="picker-icon"', 'class="picker-icon" id="pickerIcon"') : '<span class="picker-icon" id="pickerIcon"></span>';
  const q = $('#pickerSearch').value.trim().toLowerCase();
  const list = $('#pickerList');
  list.innerHTML = '';
  const shown = state.games.filter((x) => !q || x.name.toLowerCase().includes(q));
  for (const x of shown) {
    const row = h(`<div class="picker-row${x.id === state.gameId ? ' sel' : ''}" role="option">${iconHtml(x)}
      <div><b>${esc(x.name)}</b><small>${esc(x.source)}</small></div></div>`);
    row.onclick = () => { closePicker(); selectGame(x.id); };
    list.append(row);
    queueArt(x);
  }
  if (!shown.length) list.append(h(`<div class="muted small" style="padding:10px">No match.</div>`));
}

function openPicker() {
  $('#pickerMenu').hidden = false;
  $('#pickerButton').setAttribute('aria-expanded', 'true');
  $('#pickerSearch').value = '';
  renderPicker();
  $('#pickerSearch').focus();
}
function closePicker() { $('#pickerMenu').hidden = true; $('#pickerButton').setAttribute('aria-expanded', 'false'); }
$('#pickerButton').onclick = () => ($('#pickerMenu').hidden ? openPicker() : closePicker());
$('#pickerSearch').oninput = renderPicker;
document.addEventListener('mousedown', (e) => { if (!e.target.closest('#gamePicker')) closePicker(); });

async function addGame() {
  closePicker();
  const r = await call('addGame');
  if (!r || r.cancelled || r.error) return;
  state.games = r.games;
  selectGame(r.game.id);
  toast(`Added ${r.game.name}`);
}
$('#addGame').onclick = addGame;
$$('[data-action=addGame]').forEach((b) => { b.onclick = addGame; });
// Folders scanned for installs outside the stores (X:\Games, copied Steam libraries, ...).
async function manageFolders() {
  closePicker();
  let folders = await call('libraryFolders');
  if (!folders) return;
  const body = () => `<p>USPF looks for Unreal games in these folders (each game in its own subfolder), as well as in Steam, Epic and GOG.</p>
    <div class="folder-list">${folders.map((f) => `<div class="folder-row"><code>${esc(f.path)}</code>
      ${f.added ? `<button class="btn ghost small" data-rm="${esc(f.path)}">Remove</button>` : '<span class="chip">found automatically</span>'}</div>`).join('') || '<p class="muted">None yet.</p>'}</div>`;
  const m = modal({ title: 'Game folders', body: body(), actions: [{ label: '+ Add folder', value: 'add' }, { label: 'Done', value: 'done', cls: 'primary' }] });
  const wire = () => $$('#modalBody [data-rm]').forEach((b) => {
    b.onclick = async () => {
      const r = await call('removeLibraryFolder', { path: b.dataset.rm });
      if (!r) return;
      folders = r.folders; state.games = r.games;
      $('#modalBody').innerHTML = body(); wire(); renderPicker();
    };
  });
  setTimeout(wire, 0);
  const choice = await m;
  if (choice === 'add') {
    const r = await call('addLibraryFolder');
    if (!r || r.cancelled) return;
    state.games = r.games;
    renderPicker();
    toast(`${state.games.length} games found`);
    if (!currentGame()) selectGame(state.games[0]?.id);
  }
}
$('#gameFolders').onclick = manageFolders;

$('#rescan').onclick = async () => {
  closePicker();
  state.games = await call('games', { rescan: true });
  toast(`${state.games.length} games found`);
  if (!currentGame()) state.gameId = state.games[0]?.id || null;
  renderPicker();
  selectGame(state.gameId);
};

// Artwork: Steam banner when available, otherwise the exe's icon. A few at a time.
const artQueue = [];
let artBusy = 0;
function queueArt(g) {
  if (state.art[g.id] !== undefined) return;
  state.art[g.id] = null;
  artQueue.push(g);
  pumpArt();
}
function pumpArt() {
  while (artBusy < 3 && artQueue.length) {
    const g = artQueue.shift();
    artBusy++;
    api('art', { id: g.id }).then((r) => { state.art[g.id] = r; }).catch(() => { state.art[g.id] = { url: null }; }).finally(() => {
      artBusy--;
      if (!$('#pickerMenu').hidden) renderPicker(); else if (g.id === state.gameId) { renderPicker(); renderHeroArt(); }
      pumpArt();
    });
  }
}

async function selectGame(id) {
  if (state.tab === 'settings' && Object.keys(state.edits).length && id !== state.gameId) {
    const ok = await modal({ title: 'Discard unsaved settings?', body: '<p>The settings you changed for this game haven\'t been saved.</p>',
      actions: [{ label: 'Keep editing', value: false, cls: 'ghost' }, { label: 'Discard', value: true, cls: 'danger' }] });
    if (!ok) return;
  }
  state.gameId = id;
  state.edits = {};
  state.ini = null;
  state.runs = [];
  state.run = null;
  state.runId = null;
  store.set('game', id);
  renderPicker();
  const g = currentGame();
  $('#dashEmpty').hidden = !!g;
  $('#dashBody').hidden = !g;
  if (!g) return;
  queueArt(g);
  renderHeroArt();
  await refreshStatus();
  if (state.tab === 'performance') loadRuns();
  if (state.tab === 'settings') loadIni();
  if (state.tab === 'dashboard') loadRuns({ quiet: true });
}

// ---------------------------------------------------------------- dashboard
function renderHeroArt() {
  const g = currentGame();
  if (!g) return;
  const el = $('#heroArt');
  const a = state.art[g.id];
  el.className = 'hero-art';
  el.style.backgroundImage = '';
  if (a && a.url) {
    el.classList.add(a.kind === 'icon' ? 'icon-art' : 'has-art');
    el.style.backgroundImage = `url('${a.url}')`;
  }
}

function statusPill(s) {
  if (!s) return '<span class="pill none">…</span>';
  if (s.foreignDll) return '<span class="pill bad">Other d3d12.dll</span>';
  if (!s.installed) return '<span class="pill none">Not installed</span>';
  if (s.enabledInIni === false) return '<span class="pill off">DLL OFF</span>';
  if (!s.upToDate) return '<span class="pill update">DLL ON · update available</span>';
  return '<span class="pill on">DLL ON</span>';
}

async function refreshStatus() {
  const g = currentGame();
  if (!g) return;
  const s = await call('status', { id: g.id });
  if (state.gameId !== g.id) return;
  state.status = s;
  renderDashboard();
  loadCaches();
}

function alertHtml(kind, text, button) {
  const icon = kind === 'info'
    ? '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5v.5"/></svg>'
    : '<svg viewBox="0 0 24 24"><path d="M12 3 2 21h20zM12 10v5M12 18v.5"/></svg>';
  return `<div class="alert ${kind}">${icon}<div>${text}</div>${button || ''}</div>`;
}

function renderDashboard() {
  const g = currentGame(), s = state.status;
  if (!g || !s) return;
  $('#heroName').textContent = g.name;
  $('#heroSource').textContent = `${g.source}${g.isUnreal ? ' · Unreal Engine' : ''}${g.project ? ' · ' + g.project : ''}`;
  $('#heroPill').outerHTML = statusPill(s).replace('class="pill', 'id="heroPill" class="pill');
  $('#heroPath').textContent = g.exe;

  const alerts = [];
  if (s.antiCheat?.length) alerts.push(alertHtml('bad', `<b>Anti-cheat files found.</b> Use the layer only offline / single-player: an injected DLL can get an online account banned.`));
  if (s.foreignDll) alerts.push(alertHtml('warn', '<b>Another d3d12.dll is in the game folder</b> (a different mod or overlay). Installing backs it up and restores it when you uninstall.'));
  if (!g.isUnreal) alerts.push(alertHtml('info', 'This doesn\'t look like an Unreal Engine game. The layer works with any D3D12 game, but the Engine.ini settings don\'t apply.'));
  if (s.installed && !s.upToDate) alerts.push(alertHtml('info', 'This game has an older d3d12.dll than the one bundled with USPF.', '<button class="btn small" data-do="install">Update</button>'));
  $('#alerts').innerHTML = alerts.join('');

  const rows = [];
  rows.push(`<div class="row"><div class="k">Layer<small>d3d12.dll next to the game's exe</small></div><div class="v">${statusPill(s)}</div></div>`);
  if (s.installed) {
    rows.push(`<div class="row"><div class="k">Enabled<small>Off = the DLL loads but passes everything through, still logging runs</small></div>
      <label class="switch"><input type="checkbox" id="swEnabled" ${s.enabledInIni !== false ? 'checked' : ''}><span></span></label></div>`);
  }
  if (g.isUnreal) {
    const dirs = s.engineIni?.configDirs || [];
    const note = dirs.length ? 'Marked block in Engine.ini; removed on uninstall' : 'Start the game once so its config folder exists';
    rows.push(`<div class="row"><div class="k">Unreal precache settings<small>${esc(note)}</small></div>
      <select id="loadMode" title="Full: the startup precompile and loading screens compile as much as possible. Fast: they wait only for high-priority pipelines.">
        <option value="full">Full</option><option value="fast">Fast</option></select>
      <label class="switch"><input type="checkbox" id="swEngine" ${s.engineIni?.enabled ? 'checked' : ''} ${dirs.length ? '' : 'disabled'}><span></span></label></div>`);
  }
  $('#layerRows').innerHTML = rows.join('');
  const lm = $('#loadMode');
  if (lm) {
    lm.value = store.get('loadMode', 'full');
    lm.onchange = async () => {
      store.set('loadMode', lm.value);
      if ($('#swEngine').checked) { await call('engineIni', { id: g.id, enable: true, loadMode: lm.value }); toast('Engine.ini updated'); }
    };
  }
  const swE = $('#swEngine');
  if (swE) swE.onchange = async () => {
    const r = await call('engineIni', { id: g.id, enable: swE.checked, loadMode: lm.value });
    if (r && r.ok) toast(swE.checked ? 'Precache settings written to Engine.ini' : 'Precache settings removed');
    refreshStatus();
  };
  const swOn = $('#swEnabled');
  if (swOn) swOn.onchange = async () => {
    const r = await call('iniSet', { id: g.id, values: { Enabled: swOn.checked ? '1' : '0' } });
    if (r && r.ok) toast(swOn.checked ? 'Layer enabled' : 'Layer disabled (pass-through)');
    refreshStatus();
  };

  const act = [];
  if (!s.installed) act.push('<button class="btn primary" data-do="install">Install</button>');
  else {
    act.push(`<button class="btn ${s.upToDate ? '' : 'primary'}" data-do="install">${s.upToDate ? 'Reinstall' : 'Update DLL'}</button>`);
    act.push('<button class="btn danger-ghost" data-do="uninstall">Uninstall</button>');
  }
  $('#layerActions').innerHTML = act.join('');
  $('#layerNote').textContent = s.installed ? '' : 'Close the game before installing.';
  $$('[data-do=install]').forEach((b) => { b.onclick = () => install(false); });
  $$('[data-do=uninstall]').forEach((b) => { b.onclick = uninstall; });

  const folders = [
    ['Game executable folder', g.exeDir],
    ['Run logs', s.runsDir],
    ['Install folder', g.root],
    ...(s.savedDirs || []).map((d) => ['Saved folder', d]),
  ];
  $('#folders').innerHTML = folders.map(([k, p]) => `<button class="folder" data-path="${esc(p)}"><b>${esc(k)}</b><span>${esc(p)}</span></button>`).join('');
  $$('#folders .folder').forEach((b) => { b.onclick = () => call('openPath', { path: b.dataset.path }); });
  renderDashTiles();
}

function tile(label, value, unit, sub, cls = '', color = '') {
  return `<div class="tile ${cls}"><div class="label">${color ? `<i style="background:${color}"></i>` : ''}${esc(label)}</div>
    <div class="value">${value}${unit ? `<small>${unit}</small>` : ''}</div><div class="sub">${sub || '&nbsp;'}</div></div>`;
}

function renderDashTiles() {
  const r = state.runs.find((x) => x.dllOn) || state.runs[0];
  if (!r) {
    $('#dashTiles').innerHTML = tile('Last run', '—', '', 'No run logs yet: play the game with the layer installed.');
    return;
  }
  const sh = r.shaderHitches;
  $('#dashTiles').innerHTML = [
    tile('Average FPS', fmt.n(r.avgFps), 'fps', `${fmt.stamp(r.stamp)} · ${r.dllOn ? 'DLL ON' : 'DLL OFF'}`, '', 'var(--series-1)'),
    tile('1% low', fmt.n(r.low1), 'fps', '99th-percentile frame time', '', 'var(--series-2)'),
    tile('0.1% low', fmt.n(r.low01), 'fps', '99.9th-percentile frame time', '', 'var(--series-3)'),
    tile('Max FPS', fmt.n(r.maxFps), 'fps', 'best one-second average'),
    tile('Shader hitches', sh == null ? '—' : fmt.int(sh), '', `${fmt.int(r.hitches)} frames over ${fmt.int(r.hitchMs || 50)} ms in total`, sh === 0 ? 'good' : sh > 0 ? 'warn' : ''),
    tile('Startup precompile', r.startupSeconds != null ? fmt.dur(r.startupSeconds) : '—', '', r.startupPipelines != null ? `${fmt.int(r.startupPipelines)} pipelines` : ''),
  ].join('');
}

async function install(force) {
  const g = currentGame();
  const r = await call('install', { id: g.id, force, engineIni: g.isUnreal && ($('#swEngine')?.checked ?? true), loadMode: store.get('loadMode', 'full') }, { quiet: true });
  if (r && r.ok) {
    const e = r.engineIni;
    toast(e && !e.ok ? `Layer installed. Engine.ini: ${e.error}` : 'Layer installed', !!(e && !e.ok));
    return refreshStatus();
  }
  const code = r?.code;
  if (code === 'anticheat' || code === 'foreign-dll') {
    const ac = code === 'anticheat';
    const body = ac
      ? `<p>${esc(r.error)}</p><ul>${(r.files || []).map((f) => `<li><code>${esc(f)}</code></li>`).join('')}</ul>`
      : '<p>Another d3d12.dll is in the game folder (a different mod, ReShade or an overlay). USPF can back it up as <code>d3d12.dll.d3d12async.bak</code> and put it back when you uninstall.</p>';
    const go = await modal({ title: ac ? 'Anti-cheat found' : 'Replace the other d3d12.dll?', body,
      actions: [{ label: 'Cancel', value: false, cls: 'ghost' }, { label: ac ? 'Install anyway (offline only)' : 'Back up and replace', value: true, cls: ac ? 'danger' : 'primary' }] });
    if (go) return install(true);
    return;
  }
  if (code === 'access-denied' && !state.elevated) {
    const go = await modal({ title: 'Administrator rights needed', body: `<p>${esc(r.error)}</p>`,
      actions: [{ label: 'Cancel', value: false, cls: 'ghost' }, { label: 'Restart as administrator', value: true, cls: 'primary' }] });
    if (go) call('restartElevated');
    return;
  }
  toast(r?.error || 'Install failed', true);
}

async function uninstall() {
  const g = currentGame();
  const go = await modal({ title: `Uninstall from ${g.name}?`, body: '<p>Removes d3d12.dll, d3d12async.ini and the log from the game folder, and the precache block from Engine.ini. Your run history in USPF is kept.</p>',
    actions: [{ label: 'Cancel', value: false, cls: 'ghost' }, { label: 'Uninstall', value: true, cls: 'danger' }] });
  if (!go) return;
  const r = await call('uninstall', { id: g.id, engineIni: true });
  if (r && r.ok) toast(r.restoredBackup ? 'Uninstalled; the previous d3d12.dll is back' : 'Uninstalled');
  refreshStatus();
}

// ---------------------------------------------------------------- caches
async function loadCaches() {
  const g = currentGame();
  $('#cacheRows').innerHTML = '<div class="muted">Measuring…</div>';
  const list = await call('cacheList', { id: g?.id || '' }, { quiet: true }).catch(() => []);
  if (state.gameId !== g?.id) return;
  state.caches = list || [];
  const rows = state.caches.map((c) => `<div class="row"><div class="k">${esc(c.what)}<small>${esc(c.path)}</small></div>
    <div class="v">${fmt.bytes(c.bytes)}<div class="muted small">${fmt.int(c.files)} files</div></div></div>`);
  $('#cacheRows').innerHTML = rows.join('') || '<div class="muted">No shader caches found.</div>';
  $('#clearGameCache').disabled = !state.caches.some((c) => c.kind === 'game');
  $('#clearDriverCache').disabled = !state.caches.some((c) => c.kind === 'driver');
}
$('#cacheRefresh').onclick = loadCaches;
async function clearCaches(kind) {
  const g = currentGame();
  const items = state.caches.filter((c) => c.kind === kind);
  const total = items.reduce((a, c) => a + c.bytes, 0);
  const html = `<p>These will be deleted (${fmt.bytes(total)}):</p><ul>${items.map((c) => `<li>${esc(c.what)}: ${fmt.int(c.files)} files, ${fmt.bytes(c.bytes)}</li>`).join('')}</ul>
    ${kind === 'driver' ? '<p><b>Every game</b> on this PC compiles its shaders again on its next start.</p>' : '<p>The game rebuilds them; its next start takes longer.</p>'}
    <p class="muted small">Close the games first; files in use are skipped.</p>`;
  if (!(await confirmTyped(kind === 'driver' ? 'Clear GPU driver shader caches' : `Clear ${g.name}'s pipeline caches`, html))) return;
  const r = await call('cacheClear', { id: g?.id || '', kinds: [kind] });
  if (r) toast(`Deleted ${fmt.int(r.deleted)} files${r.locked ? `; ${fmt.int(r.locked)} in use were left` : ''}`);
  loadCaches();
}
$('#clearGameCache').onclick = () => clearCaches('game');
$('#clearDriverCache').onclick = () => clearCaches('driver');

// ---------------------------------------------------------------- performance
const COLORS = { avg: '#0ea5c4', low1: '#8b5cf6', low01: '#d97706', grid: '#16233b', axis: '#7d8da8', hitch: '#f87171' };

function axis(label, fmtFn) {
  return {
    stroke: COLORS.axis, grid: { stroke: COLORS.grid, width: 1 }, ticks: { stroke: COLORS.grid, width: 1 },
    font: '12px Segoe UI, system-ui', labelFont: '12px Segoe UI, system-ui', label, labelSize: label ? 20 : 0,
    values: fmtFn ? (u, vals) => vals.map(fmtFn) : undefined,
  };
}

function makeChart(key, el, opts, data) {
  if (state.charts[key]) { state.charts[key].destroy(); delete state.charts[key]; }
  el.innerHTML = '';
  if (!data || !data[0] || data[0].length < 2) { el.innerHTML = '<div class="chart-empty">Not enough data for this chart.</div>'; return; }
  const w = el.clientWidth || 800, hgt = el.clientHeight || 300;
  const u = new uPlot({ width: w, height: hgt - 34, ...opts }, data, el);
  state.charts[key] = u;
}

function resizeCharts() {
  for (const [k, u] of Object.entries(state.charts)) {
    const el = { fps: $('#fpsChart'), ft: $('#ftChart'), trend: $('#trendChart') }[k];
    if (el && el.clientWidth) u.setSize({ width: el.clientWidth, height: el.clientHeight - 34 });
  }
}
new ResizeObserver(() => resizeCharts()).observe(document.body);

const timeAxis = () => ({ ...axis('', (v) => fmt.clock(v)), space: 70 });
const cursorSync = { sync: { key: 'run' }, drag: { x: true, y: false } };

async function loadRuns({ quiet = false } = {}) {
  const g = currentGame();
  if (!g) return;
  const runs = await call('runs', { id: g.id }, { quiet }).catch(() => []);
  if (state.gameId !== g.id) return;
  state.runs = runs || [];
  renderDashTiles();
  if (state.tab !== 'performance') return;
  const sel = $('#runSelect');
  sel.innerHTML = state.runs.map((r) => `<option value="${esc(r.id)}">${esc(fmt.stamp(r.stamp))} · ${r.dllOn ? 'DLL ON' : 'DLL OFF'} · ${fmt.n(r.avgFps)} fps avg</option>`).join('');
  $('#perfEmpty').hidden = state.runs.length > 0;
  $('#perfBody').hidden = state.runs.length === 0;
  if (!state.runs.length) { $('#runPill').hidden = true; $('#runMeta').textContent = ''; return; }
  if (!state.runs.some((r) => r.id === state.runId)) state.runId = state.runs[0].id;
  sel.value = state.runId;
  renderTrend();
  loadRun(state.runId);
}
$('#runSelect').onchange = (e) => loadRun(e.target.value);
$('#runsRefresh').onclick = () => loadRuns();

async function loadRun(id) {
  const g = currentGame();
  state.runId = id;
  $('#runSelect').value = id;
  $$('#runsTable tr[data-id]').forEach((tr) => tr.classList.toggle('sel', tr.dataset.id === id));
  const d = await call('run', { id: g.id, run: id });
  if (state.runId !== id || !d) return;
  state.run = d;
  renderRun();
}

function renderRun() {
  const d = state.run, s = d.summary || {};
  const meta = state.runs.find((r) => r.id === d.id) || {};
  const pill = $('#runPill');
  pill.hidden = false;
  pill.className = 'pill ' + (meta.dllOn ? 'on' : 'off');
  pill.textContent = meta.dllOn ? 'DLL ON' : 'DLL OFF';
  $('#runMeta').textContent = [s.gameplaySeconds != null ? `${fmt.dur(s.gameplaySeconds)} of gameplay` : '', s.startupSeconds != null ? `startup precompile ${fmt.dur(s.startupSeconds)}` : ''].filter(Boolean).join(' · ');
  const sh = s.shaderHitches;
  $('#perfTiles').innerHTML = [
    tile('Average FPS', fmt.n(s.avgFps), 'fps', `${fmt.n(s.avgMs, 2)} ms per frame`, '', COLORS.avg),
    tile('Max FPS', fmt.n(meta.maxFps ?? s.maxFps), 'fps', 'best one-second average'),
    tile('1% low', fmt.n(s.low1), 'fps', '99th-percentile frame time', '', COLORS.low1),
    tile('0.1% low', fmt.n(s.low01), 'fps', '99.9th-percentile frame time', '', COLORS.low01),
    tile('Worst frame', fmt.n(s.worstMs), 'ms', `${fmt.int(s.over50)} frames over 50 ms`),
    tile('Shader hitches', sh == null ? '—' : fmt.int(sh), '', `${fmt.int(s.hitches)} hitches in total`, sh === 0 ? 'good' : sh > 0 ? 'warn' : ''),
  ].join('');

  const sec = d.seconds;
  const hasLow01 = sec.low01.some((v) => v != null);
  const fpsSeries = [{}, { label: 'Average', stroke: COLORS.avg, width: 2, value: (u, v) => (v == null ? '—' : v.toFixed(1) + ' fps') },
    { label: '1% low', stroke: COLORS.low1, width: 1.5, value: (u, v) => (v == null ? '—' : v.toFixed(1) + ' fps') }];
  const fpsData = [sec.t, sec.fps, sec.low1];
  if (hasLow01) { fpsSeries.push({ label: '0.1% low', stroke: COLORS.low01, width: 1.5, value: (u, v) => (v == null ? '—' : v.toFixed(1) + ' fps') }); fpsData.push(sec.low01); }
  makeChart('fps', $('#fpsChart'), {
    series: fpsSeries.map((x, i) => (i ? { points: { show: false }, ...x } : { label: 'Time', value: (u, v) => (v == null ? '—' : fmt.clock(v)) })),
    axes: [timeAxis(), axis('fps')], cursor: cursorSync, scales: { x: { time: false }, y: { range: (u, mn, mx) => [0, Math.max(30, mx * 1.08)] } },
  }, fpsData);

  const fr = d.frames;
  if (fr && fr.t.length > 1) {
    $('#ftNote').textContent = d.framesThinned ? 'Every frame (long run: thinned, keeping the slower frame of each pair).' : 'Every gameplay frame.';
    makeChart('ft', $('#ftChart'), {
      series: [{ label: 'Time', value: (u, v) => (v == null ? '—' : fmt.clock(v)) }, { label: 'Frame time', stroke: COLORS.avg, width: 1, points: { show: false }, value: (u, v) => (v == null ? '—' : v.toFixed(2) + ' ms') }],
      axes: [timeAxis(), axis('ms')], cursor: cursorSync, scales: { x: { time: false }, y: { range: (u, mn, mx) => [0, Math.max(20, Math.min(mx * 1.05, 400))] } },
    }, [fr.t, fr.ms]);
  } else {
    $('#ftNote').textContent = 'This run predates per-frame logs: slowest and average frame of each second.';
    makeChart('ft', $('#ftChart'), {
      series: [{ label: 'Time', value: (u, v) => (v == null ? '—' : fmt.clock(v)) },
        { label: 'Slowest frame', stroke: COLORS.low01, width: 1.5, points: { show: false }, value: (u, v) => (v == null ? '—' : v.toFixed(1) + ' ms') },
        { label: 'Average', stroke: COLORS.avg, width: 1.5, points: { show: false }, value: (u, v) => (v == null ? '—' : v.toFixed(2) + ' ms') }],
      axes: [timeAxis(), axis('ms')], cursor: cursorSync, scales: { x: { time: false }, y: { range: (u, mn, mx) => [0, Math.max(20, Math.min(mx * 1.05, 400))] } },
    }, [sec.t, sec.maxMs, sec.avgMs]);
  }

  const hs = d.hitches || [];
  $('#hitchNote').textContent = hs.length ? 'Click a row to zoom the charts to it; double-click a chart to zoom out.' : '';
  const causeName = { 'not-pipeline': 'Not shaders', compile: 'Shader compile', 'forced-wait': 'Waited for a shader', 'several-calls': 'Several shader calls' };
  $('#hitchTable').innerHTML = hs.length
    ? `<thead><tr><th>Time</th><th class="num">Frame</th><th>Cause</th></tr></thead><tbody>${hs.map((x, i) => `<tr class="click" data-i="${i}">
      <td>${fmt.clock(x.t)}</td><td class="num">${fmt.n(x.ms, 0)} ms</td><td title="${esc(x.detail)}"><span class="cause ${x.cause === 'not-pipeline' ? '' : 'shader'}">${esc(causeName[x.cause] || x.cause)}</span></td></tr>`).join('')}</tbody>`
    : '<tbody><tr><td class="muted">No hitches in this run.</td></tr></tbody>';
  $$('#hitchTable tr.click').forEach((tr) => {
    tr.onclick = () => {
      const x = hs[+tr.dataset.i];
      for (const k of ['fps', 'ft']) state.charts[k]?.setScale('x', { min: x.t - 10, max: x.t + 10 });
    };
  });
  $('#runText').textContent = d.text || '';
}

function renderTrend() {
  const runs = [...state.runs].reverse();
  const xs = runs.map((_, i) => i);
  makeChart('trend', $('#trendChart'), {
    series: [{ label: 'Run', value: (u, v) => (v == null ? '—' : fmt.stamp(runs[v]?.stamp)) },
      { label: 'Average', stroke: COLORS.avg, width: 2, points: { size: 8, fill: COLORS.avg }, value: (u, v) => (v == null ? '—' : v.toFixed(1) + ' fps') },
      { label: '1% low', stroke: COLORS.low1, width: 2, points: { size: 8, fill: COLORS.low1 }, value: (u, v) => (v == null ? '—' : v.toFixed(1) + ' fps') },
      { label: '0.1% low', stroke: COLORS.low01, width: 2, points: { size: 8, fill: COLORS.low01 }, value: (u, v) => (v == null ? '—' : v.toFixed(1) + ' fps') }],
    axes: [{ ...axis('', (v) => (Number.isInteger(v) && runs[v] ? fmt.stamp(runs[v].stamp).slice(5) : '')), space: 90, incrs: [1, 2, 5, 10, 20, 50] }, axis('fps')],
    scales: { x: { time: false, range: (u, mn, mx) => [mn - 0.3, mx + 0.3] }, y: { range: (u, mn, mx) => [0, Math.max(30, mx * 1.1)] } },
    cursor: { drag: { x: false, y: false } },
  }, xs.length >= 2 ? [xs, runs.map((r) => r.avgFps ?? null), runs.map((r) => r.low1 ?? null), runs.map((r) => r.low01 ?? null)] : null);
  if (runs.length < 2) $('#trendChart').innerHTML = '<div class="chart-empty">The trend appears once there are two or more runs.</div>';
  $('#runsTable').innerHTML = `<thead><tr><th>Run</th><th>Layer</th><th class="num">Avg</th><th class="num">Max</th><th class="num">1% low</th><th class="num">0.1% low</th><th class="num">Shader hitches</th><th class="num">Gameplay</th></tr></thead><tbody>${state.runs.map((r) => `
    <tr class="click${r.id === state.runId ? ' sel' : ''}" data-id="${esc(r.id)}"><td>${esc(fmt.stamp(r.stamp))}</td><td><span class="pill ${r.dllOn ? 'on' : 'off'}">${r.dllOn ? 'ON' : 'OFF'}</span></td>
    <td class="num">${fmt.n(r.avgFps)}</td><td class="num">${fmt.n(r.maxFps)}</td><td class="num">${fmt.n(r.low1)}</td><td class="num">${fmt.n(r.low01)}</td>
    <td class="num">${r.shaderHitches == null ? '—' : fmt.int(r.shaderHitches)}</td><td class="num">${fmt.dur(r.gameplaySeconds)}</td></tr>`).join('')}</tbody>`;
  $$('#runsTable tr[data-id]').forEach((tr) => { tr.onclick = () => loadRun(tr.dataset.id); });
}

// ---------------------------------------------------------------- settings
// Layout and control types for d3d12async.ini keys. Help text comes from the ini's own comments.
const GROUPS = [
  ['General', ['Enabled', 'BudgetMs', 'AsyncThreads']],
  ['Queue & placeholders', ['MaxQueuedCompiles', 'QueueFullMode', 'MaxPlaceholders', 'WorkerEfficiencyMode', 'MaxCompilesPerSecond']],
  ['Compute pipelines', ['ComputePipelineMode']],
  ['Startup precompile', ['StartupCompileNormally', 'StartupFpsCap', 'StartupMinSeconds', 'StartupMinPsos', 'StartupQuietSeconds', 'StartupQuietRate',
    'StartupCpuBusyPercent', 'StartupFloodSeconds', 'StartupFloodWaitSeconds', 'StartupHotkey', 'StartupMaxSeconds', 'StartupBoostCompileThreads']],
  ['Loading screens', ['LoadingCompileNormally', 'LoadingGapMs', 'LoadingHoldMs']],
  ['UI pipelines', ['UiPipelineMode', 'UiWaitMaxMs']],
  ['Workers & CPU affinity', ['Workers', 'BurstWorkers', 'BurstQueue', 'LowPriorityWorkers', 'WorkerCpus']],
  ['Overlay', ['Overlay', 'OverlayStats', 'OverlayOnlyWhenFocused', 'OverlayOpacity', 'OverlayGraph', 'SettingsPanel', 'HitchMs']],
  ['Run logs & diagnostics', ['RunLogs', 'RunLogsKeepOn', 'RunLogsKeepOff', 'MeasureWhenDisabled', 'LogLevel', 'SlowCompileLogMs', 'DebugCompileDelayMs']],
];
const BASIC = new Set(['Enabled', 'QueueFullMode', 'MaxPlaceholders', 'WorkerEfficiencyMode', 'ComputePipelineMode', 'StartupFpsCap', 'StartupMinSeconds', 'UiWaitMaxMs', 'Overlay', 'RunLogs']);
const BOOLS = new Set(['Enabled', 'WorkerEfficiencyMode', 'StartupCompileNormally', 'StartupHotkey', 'StartupBoostCompileThreads', 'LoadingCompileNormally',
  'LowPriorityWorkers', 'Overlay', 'OverlayStats', 'OverlayOnlyWhenFocused', 'OverlayGraph', 'SettingsPanel', 'RunLogs', 'MeasureWhenDisabled']);
const ENUMS = {
  AsyncThreads: [['all', 'All threads'], ['recording', 'Recording threads only']],
  QueueFullMode: [['compile', 'Compile'], ['placeholder', 'Placeholder']],
  ComputePipelineMode: [['sync', 'Sync'], ['wait', 'Wait'], ['skip', 'Skip']],
  UiPipelineMode: [['wait', 'Wait'], ['sync', 'Sync'], ['async', 'Async']],
  LogLevel: [['error', 'Error'], ['warn', 'Warning'], ['info', 'Info'], ['debug', 'Debug']],
};
const RANGES = { OverlayOpacity: [30, 255], StartupCpuBusyPercent: [0, 100] };
const TEXTS = new Set(['WorkerCpus', 'BurstWorkers']);
const EXPERIMENTAL = new Set(['WorkerEfficiencyMode', 'MaxCompilesPerSecond', 'ComputePipelineMode']);

async function loadIni() {
  const g = currentGame();
  if (!g) return;
  if (state.ini && state.ini.gameId === g.id) { renderSettings(); return; }
  const r = await call('iniGet', { id: g.id });
  if (state.gameId !== g.id || !r) return;
  state.ini = { ...r, gameId: g.id };
  state.edits = {};
  renderSettings();
}

function valueOf(key) {
  if (key in state.edits) return state.edits[key];
  const v = state.ini.values[key];
  if (v !== undefined) return v;
  return state.ini.schema.find((s) => s.key === key)?.default ?? '';
}

function control(key, def) {
  const v = valueOf(key);
  if (BOOLS.has(key)) return `<label class="switch"><input type="checkbox" data-key="${key}" ${v !== '0' ? 'checked' : ''}><span></span></label>`;
  if (ENUMS[key]) {
    const opts = [...ENUMS[key]];
    if (!opts.some(([o]) => o === v.toLowerCase())) opts.push([v, v]);
    return `<select data-key="${key}">${opts.map(([o, l]) => `<option value="${esc(o)}" ${o === v.toLowerCase() ? 'selected' : ''}>${esc(l)}</option>`).join('')}</select>`;
  }
  if (RANGES[key]) {
    const [mn, mx] = RANGES[key];
    return `<input type="range" min="${mn}" max="${mx}" data-key="${key}" value="${esc(v)}"><input class="input num" data-key="${key}" value="${esc(v)}" inputmode="numeric">`;
  }
  if (TEXTS.has(key)) return `<input class="input" style="width:140px" data-key="${key}" value="${esc(v)}" spellcheck="false">`;
  return `<input class="input num" data-key="${key}" value="${esc(v)}" inputmode="decimal">`;
}

function renderSettings() {
  const ini = state.ini, s = state.status;
  const installed = s && s.installed;
  $('#settingsEmpty').hidden = installed;
  $('#settingsGrid').hidden = !installed;
  $('#iniPath').textContent = installed ? ini.path : '';
  $$('#settingsMode button').forEach((b) => b.classList.toggle('active', b.dataset.mode === state.settingsMode));
  $('#iniSave').disabled = !installed || !Object.keys(state.edits).length;
  $('#iniReset').disabled = !installed;
  $('#iniOpen').disabled = !installed;
  if (!installed) return;
  const schema = new Map(ini.schema.map((k) => [k.key, k]));
  const listed = new Set(GROUPS.flatMap(([, ks]) => ks));
  const groups = [...GROUPS, ['Other', ini.schema.map((k) => k.key).filter((k) => !listed.has(k))]];
  const basic = state.settingsMode === 'basic';
  let n = 0;
  $('#settingsGrid').innerHTML = groups.map(([title, keys]) => {
    const ks = keys.filter((k) => schema.has(k) && (!basic || BASIC.has(k)));
    if (!ks.length) return '';
    n++;
    return `<div class="card"><div class="card-head"><span class="group-num">${n}</span><h3>${esc(title)}</h3></div>
      ${ks.map((k) => {
        const sk = schema.get(k);
        const changed = k in state.edits;
        const tip = `${sk.help || 'No description.'}\n\nDefault: ${sk.default}`;
        return `<div class="set-row${changed ? ' changed' : ''}"><div class="name"><code>${esc(k)}</code>${EXPERIMENTAL.has(k) ? '<span class="badge">experimental</span>' : ''}
          <span class="help" data-tip="${esc(tip)}">?</span>${valueOf(k) !== sk.default ? `<span class="default-note">default ${esc(sk.default)}</span>` : ''}</div>
          <div class="ctl">${control(k, sk.default)}</div></div>`;
      }).join('')}</div>`;
  }).join('');
  $$('#settingsGrid [data-key]').forEach((el) => {
    const ev = el.type === 'checkbox' || el.tagName === 'SELECT' ? 'change' : 'input';
    el.addEventListener(ev, () => {
      const key = el.dataset.key;
      let v = el.type === 'checkbox' ? (el.checked ? '1' : '0') : el.value.trim();
      if (el.type === 'range' || (RANGES[key] && el.classList.contains('num'))) $$(`[data-key="${key}"]`).forEach((o) => { if (o !== el) o.value = v; });
      const orig = state.ini.values[key] ?? schema.get(key).default;
      if (v === orig) delete state.edits[key]; else state.edits[key] = v;
      el.closest('.set-row').classList.toggle('changed', key in state.edits);
      $('#iniSave').disabled = !Object.keys(state.edits).length;
    });
  });
}

$$('#settingsMode button').forEach((b) => {
  b.onclick = () => { state.settingsMode = b.dataset.mode; store.set('settingsMode', state.settingsMode); renderSettings(); };
});
$('#iniSave').onclick = async () => {
  const g = currentGame();
  const bad = Object.entries(state.edits).filter(([k, v]) => !TEXTS.has(k) && !ENUMS[k] && !BOOLS.has(k) && !/^-?\d+(\.\d+)?$/.test(v));
  if (bad.length) { toast(`Not a number: ${bad.map(([k]) => k).join(', ')}`, true); return; }
  const r = await call('iniSet', { id: g.id, values: state.edits });
  if (r && r.ok) {
    toast('Settings saved. They apply the next time the game starts.');
    state.ini = null;
    state.edits = {};
    await loadIni();
    refreshStatus();
  }
};
$('#iniReset').onclick = async () => {
  const g = currentGame();
  const go = await modal({ title: 'Reset to defaults?', body: '<p>Replaces this game\'s d3d12async.ini with the defaults bundled with USPF.</p>',
    actions: [{ label: 'Cancel', value: false, cls: 'ghost' }, { label: 'Reset', value: true, cls: 'danger' }] });
  if (!go) return;
  const r = await call('iniReset', { id: g.id });
  if (r && r.ok) { toast('Defaults restored'); state.ini = null; state.edits = {}; loadIni(); refreshStatus(); }
};
$('#iniOpen').onclick = () => call('openPath', { path: state.ini?.path || '' });

// ---------------------------------------------------------------- about / updates
$('#checkUpdate').onclick = async () => {
  const btn = $('#checkUpdate');
  btn.disabled = true;
  btn.textContent = 'Checking…';
  try {
    const r = await call('checkUpdate', {}, { quiet: true });
    const body = $('#updateBody');
    $('#updateCard').hidden = false;
    if (r.status === 'available') {
      body.innerHTML = `<p>Version <b>${esc(r.latest)}</b> is available (you have ${esc(r.current)}).</p>
        ${r.notes ? `<pre class="summary">${esc(r.notes)}</pre>` : ''}
        <div class="actions">${r.assetUrl ? '<button class="btn primary" id="doUpdate">Download and restart</button>' : ''}
        <button class="btn ghost" id="openRelease">Open release page</button></div>`;
      $('#doUpdate') && ($('#doUpdate').onclick = async () => {
        $('#doUpdate').disabled = true;
        const u = await call('applyUpdate', { url: r.assetUrl });
        if (u && u.ok) toast('Restarting with the new version…');
      });
      $('#openRelease').onclick = () => call('openUrl', { url: r.pageUrl || r.releasesUrl });
    } else if (r.status === 'current') {
      body.innerHTML = `<p>You have the latest version (${esc(r.current)}).</p>`;
    } else if (r.status === 'none') {
      body.innerHTML = '<p>No releases have been published yet.</p>';
    } else {
      body.innerHTML = `<p>Couldn't check for updates: ${esc(r.error || 'unknown error')}</p>`;
    }
  } catch (e) {
    toast(e.message, true);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Check for updates';
  }
};

// ---------------------------------------------------------------- start
(async function init() {
  const r = await call('init');
  state.version = r.version;
  state.elevated = r.elevated;
  state.games = r.games;
  $('#aboutVersion').textContent = `Version ${state.version}`;
  if (r.github) $('#githubLink').onclick = () => call('openUrl', { url: r.github });
  if (state.elevated) $('#topStatus').innerHTML = '<span class="pill update" title="USPF is running with administrator rights">Administrator</span>';
  if (!currentGame()) state.gameId = state.games[0]?.id || null;
  renderPicker();
  await selectGame(state.gameId);
  const tab = store.get('tab', 'dashboard');
  if (tab !== 'dashboard') showTab(tab);
})();
