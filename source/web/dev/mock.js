// Development-only backend for previewing the UI in a normal browser. Not embedded in USPF.exe.
(function () {
  const runs = Object.values(window.MOCK_RUNS).filter((r) => !r.meta.id.startsWith('2026-10-14'));
  // A few older runs (scaled copies) so the trend chart has something to show.
  const base = runs.find((r) => r.meta.id.startsWith('2026-10-10'));
  const extra = [['2026-10-09_21-10-02', 0.82, false, 34], ['2026-10-10_02-44-51', 0.91, true, 23], ['2026-10-10_05-12-17', 0.95, true, 9]].map(([st, k, on, sh]) => {
    const id = `${st}_dll-${on ? 'ON' : 'OFF'}`;
    const m = { ...base.meta, id, stamp: st, dllOn: on, avgFps: +(base.meta.avgFps * k).toFixed(1), low1: +(base.meta.low1 * k * k).toFixed(1),
      low01: +(base.meta.low01 * k * k * k).toFixed(1), maxFps: +(base.meta.maxFps * k).toFixed(1), shaderHitches: sh, hitches: sh + 4 };
    return { meta: m, detail: { ...base.detail, id, summary: m } };
  });
  const all = [...runs, ...extra].sort((a, b) => (a.meta.id < b.meta.id ? 1 : -1));
  const games = [
    { id: 'a1', name: 'Borderlands 4', source: 'steam', appid: '1285190', exe: 'F:\\Games\\Borderlands 4\\OakGame\\Binaries\\Win64\\Borderlands4.exe', exeDir: 'F:\\Games\\Borderlands 4\\OakGame\\Binaries\\Win64', root: 'F:\\Games\\Borderlands 4', project: 'OakGame', isUnreal: true },
    { id: 'b2', name: "Tiny Tina's Wonderlands", source: 'epic', appid: '', exe: 'D:\\Epic\\TinyTina\\OakGame\\Binaries\\Win64\\Wonderlands.exe', exeDir: 'D:\\Epic\\TinyTina\\OakGame\\Binaries\\Win64', root: 'D:\\Epic\\TinyTina', project: 'OakGame', isUnreal: true },
    { id: 'c3', name: 'STAR WARS Jedi: Survivor', source: 'manual', appid: '', exe: 'C:\\Games\\STAR WARS Jedi - Survivor\\SwGame\\Binaries\\Win64\\JediSurvivor.exe', exeDir: 'C:\\Games\\STAR WARS Jedi - Survivor\\SwGame\\Binaries\\Win64', root: 'C:\\Games\\STAR WARS Jedi - Survivor', project: 'SwGame', isUnreal: true },
  ];
  const parseIni = (t) => {
    const out = []; let pending = '', active = '', prevKey = false;
    for (const raw of t.split(/\r?\n/)) {
      const l = raw.trim();
      if (!l || (l.startsWith('[') && l.endsWith(']'))) { pending = ''; active = ''; prevKey = false; continue; }
      if (l.startsWith(';')) { if (prevKey) { pending = ''; prevKey = false; } pending += (pending ? '\n' : '') + l.slice(1).replace(/^ /, ''); continue; }
      const eq = l.indexOf('='); if (eq < 0) continue;
      if (pending) { active = pending; pending = ''; } else if (!prevKey) active = '';
      out.push({ key: l.slice(0, eq).trim(), default: l.slice(eq + 1).trim(), help: active }); prevKey = true;
    }
    return out;
  };
  const schema = parseIni(window.MOCK_INI);
  const values = { a1: Object.fromEntries(schema.map((s) => [s.key, s.default])) };
  values.a1.MaxPlaceholders = '256';
  const installed = { a1: true, b2: false, c3: true };
  const wait = (v, ms = 120) => new Promise((r) => setTimeout(() => r(structuredClone(v)), ms));
  window.USPF_MOCK = (m, p) => {
    const g = games.find((x) => x.id === p.id);
    switch (m) {
      case 'init': return wait({ version: '0.1.0', elevated: false, games });
      case 'games': return wait(games);
      case 'art': return wait(p.id === 'c3' ? { url: '../logo.svg', kind: 'icon' } : { url: null });
      case 'status': return wait({ ...g, dllPresent: installed[g.id], installed: installed[g.id], foreignDll: false, upToDate: g.id !== 'c3',
        iniPresent: installed[g.id], enabledInIni: (values[g.id]?.Enabled ?? '1') !== '0', antiCheat: [],
        engineIni: { configDirs: [{ dir: 'C:\\Users\\RedMatter\\Documents\\My Games\\Borderlands 4\\Saved\\Config\\Windows', hasBlock: true }], enabled: g.id === 'a1' },
        savedDirs: ['C:\\Users\\RedMatter\\Documents\\My Games\\Borderlands 4\\Saved'], runsDir: g.exeDir + '\\d3d12async-runs' });
      case 'runs': return wait(g.id === 'b2' ? [] : all.map((r) => r.meta));
      case 'run': return wait(all.find((r) => r.meta.id === p.run).detail, 200);
      case 'iniGet': return wait({ schema, values: values[g.id] || Object.fromEntries(schema.map((s) => [s.key, s.default])), present: true, path: g.exeDir + '\\d3d12async.ini' });
      case 'iniSet': values[g.id] = { ...(values[g.id] || {}), ...p.values }; return wait({ ok: true });
      case 'iniReset': delete values[g.id]; return wait({ ok: true });
      case 'cacheList': return wait([
        { kind: 'driver', what: 'NVIDIA D3D shader cache (all games)', path: 'C:\\Users\\RedMatter\\AppData\\Local\\NVIDIA\\DXCache', files: 412, bytes: 3.1e9 },
        { kind: 'game', what: 'Unreal D3D12 pipeline cache (*.ushaderprecache)', path: 'C:\\Users\\RedMatter\\AppData\\Local\\Borderlands 4\\Saved', files: 3, bytes: 199e6 }]);
      case 'cacheClear': return wait({ ok: true, deleted: 3, locked: 0 });
      case 'install': installed[g.id] = true; return wait({ ok: true });
      case 'uninstall': installed[g.id] = false; return wait({ ok: true });
      case 'engineIni': return wait({ ok: true });
      case 'libraryFolders': return wait([{ path: 'E:\\MyGames', added: true, exists: true }, { path: 'C:\\Games', added: false, exists: true },
        { path: 'C:\\Games\\steamapps\\common', added: false, exists: true }, { path: 'D:\\Games', added: false, exists: true }, { path: 'F:\\Games', added: false, exists: true }]);
      case 'checkUpdate': return wait({ status: 'none', current: '0.1.0' }, 600);
      default: return wait({ ok: true });
    }
  };
})();
