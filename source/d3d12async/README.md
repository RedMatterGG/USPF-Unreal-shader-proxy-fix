# d3d12async

A drop-in `d3d12.dll` for Windows that stops D3D12 games from **freezing** when a shader/PSO has to compile mid-game. Instead of blocking the frame, the pipeline compiles in the background and the objects that need it are **not drawn for a few frames** until it's ready. This is the D3D12 counterpart of DXVK's "async" patches and of what Dolphin achieves with ubershaders.

It doesn't make compilation faster. It moves it off the critical path.

**`FEATURES.md`** explains every feature, what happens during a launch, the on-screen label states, the run logs, troubleshooting by symptom, and all settings.

## How it works

1. The game calls `CreateGraphicsPipelineState` / `CreatePipelineState`.
2. The description is deep-copied and the real compile is queued on background threads.
3. If the compile finishes within `BudgetMs` (1 ms default; driver-cache hits take well under 1 ms), the game gets the real PSO as usual.
4. Otherwise the game gets a **placeholder** PSO immediately, and doesn't freeze.
5. While a placeholder is bound on a command list, its `Draw*`, `DispatchMesh`, `ExecuteIndirect` and `ExecuteBundle` calls are dropped. As soon as the real PSO exists, it is used everywhere the placeholder was.
6. A placeholder the game actually tries to draw with jumps to the front of the compile queue.

**Never deferred:**
- Anything created during the game's **startup precompile** (`StartupCompileNormally=1`). On a warm launch (driver cache already full) the phase ends as soon as the startup pipelines turn out to be cache hits. The layer then compiles normally from launch and watches the stream of PSO creations; once it has seen at least `StartupMinPsos` and then `StartupQuietSeconds` seconds in a row with at most `StartupQuietRate` new PSOs each, the precompile is over and background compiling switches on, without a restart. During that phase the compiling threads get the highest priority and the frame rate is capped (`StartupFpsCap`).
- **UI-type pipelines are never skipped**: only 8-bit colour targets (`R8G8B8A8`, `B8G8R8A8`, `R10G10B10A2`, …) and no depth target. Menus, text and widgets are often drawn once into a cached texture and reused, so a skipped draw there would stay empty (the empty Borderlands 4 settings menu). They compile in the background, and a draw that needs one waits for it (`UiPipelineMode=wait`).
- **Compute PSOs** (`ComputePipelineMode`): by default (`skip`) they compile in the background and dispatches that use one are skipped until it's ready. Skipped compute work (culling, Nanite, Lumen) can cause brief glitches; in Borderlands 4 none were seen. `sync` compiles them on the calling thread as without the DLL; `wait` compiles them in the background and makes a dispatch wait.
- Bundles.
- Anything that needs the real object: residency calls, `GetCachedBlob`, unknown interfaces. These wait for the real PSO.
- `ID3D12PipelineLibrary::StorePipeline` of a PSO that is still compiling (Unreal stores every new PSO into its cache library) returns immediately; the real PSO is stored once its compile finishes. Until then a load of that name misses, which the engine already handles.

Nothing on disk is modified apart from the DLL you place. Hooks are in-memory only, on the game's own D3D12 objects.

Some drivers give each command list a private vtable whose entries point straight into the driver (NVIDIA does this in some games). Any vtable the hooks haven't seen before is hooked in full the moment it is first used, and checked again after every `Reset`. If tracking ever overflows, placeholders switch off for the session instead of risking a crash.

## On-screen label

A small label on the left side of the game window confirms the layer is loaded:

> **d3d12async active**
> configure in d3d12async.ini
> compiling 0 · last 1 s: 4 new, 3 in background, 2 ms blocked, 0 draws skipped
> hitches 19 (1 compile) · last 301 ms, 3 s ago: forced wait for a background compile: 295 ms on a render thread
> *(frame-time graph: green normal, amber hitch, red hitch caused by a pipeline compile)*

Every gameplay frame slower than `HitchMs` (50 ms) is recorded as a hitch with its likely cause: which pipeline call blocked the frame and why it wasn't compiled in the background, or "no pipeline compile" when the hitch is streaming, CPU or GPU work the DLL can't fix. Hitches go to the label, `d3d12async.log` and the run's `_hitches.csv`. See `FEATURES.md`.

- It is a separate click-through window that follows the game. It never draws into the game's frames.
- It's visible in windowed and borderless mode, and hidden in exclusive fullscreen.
- It hides while the game isn't the foreground window.
- Turn it off with `Overlay=0`, drop the counters with `OverlayStats=0`, hide the graph with `OverlayGraph=0`, or change its transparency with `OverlayOpacity`.

## Install

Use **USPF** (`USPF.exe`), the app that installs and manages this DLL per game: install,
uninstall, the Unreal `Engine.ini` settings below, shader-cache clearing, settings and run logs.
It refuses games with anti-cheat unless you confirm, and backs up another `d3d12.dll` it would
replace.

By hand: copy `d3d12.dll` and `d3d12async.ini` next to the game's real exe (for Unreal games
`<Project>\Binaries\Win64\*-Shipping.exe`, not the launcher in the install root). To uninstall,
delete both.

**Clearing shader caches** (USPF: Dashboard → caches; it lists every cache with its size and
deletes only after you type `YES`):
- The game's own caches, in every `Saved` folder that belongs to it: under `%LOCALAPPDATA%`,
  `Documents\My Games`, `Documents` or the install folder. Some games split it: Tiny Tina's
  Wonderlands and Borderlands 4 keep settings in `Documents\My Games\<Game>\Saved` but the
  pipeline cache in `%LOCALAPPDATA%\<Game>\Saved`.
  - `*.ushaderprecache`: Unreal's D3D12 pipeline cache. Clearing only the driver cache leaves
    this one warm, so a run isn't really cold.
  - `*.upipelinecache`: the PSO list Unreal recorded while you played; it feeds the startup precompile.
- GPU driver caches (`NVIDIA\DXCache`, `AMD\DxcCache`, `AMD\DxCache`, Windows `D3DSCache`).
  They are shared by all games, so every game recompiles on its next launch.
- For a fair ON/OFF comparison, clear both kinds before each run. Saves and settings are never touched.

### Unreal Engine settings (`[SystemSettings]`)

UE 5.2+ has its own version of "don't draw until the PSO is ready", but only for PSOs it predicted. USPF's Engine.ini option writes this profile into the per-user `Engine.ini`, which needs no `.pak`. It makes the startup precompile and loading screens compile **as much as possible** and wait until it's done. Startup takes longer, but far fewer compiles are left for gameplay; the DLL handles the rest in the background:

```
r.PSOPrecaching=1
r.PSOPrecache.GlobalShaders=1
r.PSOPrecache.Components=1
r.PSOPrecaching.WaitForHighPriorityRequestsOnly=0   ; loading waits for every precached PSO (Fast sets 1)
r.PSOPrecache.ProxyCreationWhenPSOReady=1
r.PSOPrecache.ProxyCreationDelayStrategy=0          ; 1 = draw with the default material instead of nothing
r.PSOPrecache.KeepInMemoryUntilUsed=1               ; NVIDIA: keep precached PSOs in memory until first use
r.ShaderPipelineCache.ExcludePrecachePSO=0          ; the bundled-cache startup pass compiles everything in it (Fast sets 1)
r.PSOPrecache.Resources=1                           ; also precache per-resource PSOs: fewer misses later (left out in Fast)
```

It never turns off the game's bundled PSO cache (`r.ShaderPipelineCache.Enabled`) or its own warm-up pass (`r.PSOWarmup.WarmupMaterials`): that made some games' "compiling shaders" screen finish instantly and let the game continue before compiling was done. Applying it again replaces the old lines.

**Fast** is the shorter-loading profile: loading waits only for high-priority PSOs, the bundled-cache pass skips PSOs the precacher covers, and no per-resource precaching. More compiles are left for gameplay.

Some games run a fully custom compile screen in their own code, which no cvar changes. Whatever isn't compiled at startup is compiled in the background by the DLL during play, which shows up as brief pop-in rather than freezes.

**Why not a single `.pak` for every game:**
- A pak's mount path is tied to each game's project name.
- A config file in a pak *replaces* the game's file of the same name instead of merging with it.
- Many games only load signed paks.
- The pak format version differs between engine versions.

The per-user `Engine.ini` works in most UE games without any of that. A per-game pak is only worth making for a game that ignores the user `Engine.ini`.

The UE settings and the layer complement each other. UE handles the PSOs it precached. d3d12async catches the misses UE didn't predict, which are what still freeze.

## Configuration (`d3d12async.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | 0 turns the layer into a pure pass-through |
| `BudgetMs` | 1 | How long a create may block before a placeholder is handed out. Skipped when compiles are already queued (the new one couldn't start in time anyway), so a burst of hundreds of creates in one frame doesn't stall |
| `AsyncThreads` | all | `all` is needed for UE5, which creates runtime PSOs on task threads. `recording` limits placeholders to threads that record command lists |
| `MaxQueuedCompiles` | 256 | Backpressure: past this many queued compiles, non-recording (precache) threads block as usual |
| `StartupCompileNormally` | 1 | Compile normally during the game's startup precompile; switch to background compiling when it ends |
| `StartupMinPsos` / `StartupQuietSeconds` / `StartupQuietRate` | 64 / 10 / 10 | The precompile counts as over after this many PSOs, followed by this many seconds with at most this many new PSOs each |
| `StartupCpuBusyPercent` | 80 | A second with game CPU use above this also counts as busy; 0 = ignore CPU |
| `LoadingCompileNormally` / `LoadingGapMs` / `LoadingHoldMs` | 0 / 500 / 1500 | Experimental, off: while no frame is presented for 500 ms (a loading screen), pipelines compile normally, and for 1.5 s after frames resume |
| `StartupMinSeconds` | 40 | For this long after launch only the hotkey can end the startup phase |
| `StartupWarmLaunch` / `StartupSlowCompileMs` | 1 / 5 | Warm launch: when the startup pipelines are driver-cache hits (under `StartupSlowCompileMs` each), the phase ends right away |
| `StartupFloodSeconds` / `StartupFloodWaitSeconds` | 10 / 45 | The phase can't end before the precompile has been seen (busy seconds after the first frame), unless none starts within the wait (warm caches) |
| `StartupHotkey` | 1 | Ctrl+Shift+End ends the startup phase by hand |
| `StartupMaxSeconds` | 0 | Optional hard limit on the startup phase |
| `StartupBoostCompileThreads` | 1 | During the startup phase, the game thread doing a compile runs at highest priority for that compile |
| `StartupFpsCap` | 30 | Frame-rate cap only while the startup precompile runs, so rendering leaves more CPU to compiling; 0 = off |
| `RunLogs` / `RunLogsKeepOn` / `RunLogsKeepOff` | 1 / 1 / 1 | Per-run frame-time logs; how many runs to keep with the layer on and with it off |
| `MeasureWhenDisabled` | 1 | With `Enabled=0` the DLL changes nothing but still measures, for dll-OFF run logs |
| `UiWaitMaxMs` | 2 | Cap on how long a draw waits for a UI pipeline before it's skipped (0 = no cap) |
| `UiPipelineMode` | wait | UI-type pipelines (8-bit targets, no depth) are never skipped: compiled in the background, and a draw that needs one waits for it (`sync` = compile on the calling thread, `async` = skip like any other) |
| `Workers` | 2 | Compile threads; 0 = a quarter of hardware threads, clamped to 2–8 |
| `BurstWorkers` / `BurstQueue` | auto / 16 | Extra compile threads that only work while more than `BurstQueue` compiles are waiting; auto = CPUs allowed by `WorkerCpus` (max 8) |
| `QueueFullMode` | placeholder | `compile` = past `MaxQueuedCompiles`, compile on the game's thread; `placeholder` = past `MaxQueuedCompiles`, still hand out placeholders instead of compiling on the game's thread |
| `MaxPlaceholders` | 512 | With `QueueFullMode=placeholder`: cap on outstanding placeholders past the queue limit |
| `SettingsPanel` | 0 | Bottom-left settings panel; Ctrl+Shift+P toggles queue-full placeholders live |
| `WorkerEfficiencyMode` | 1 | compile threads in Windows 11 efficiency mode (EcoQoS) |
| `MaxCompilesPerSecond` | 0 | Experimental: cap on background compiles started per second (0 = none) |
| `ComputePipelineMode` | skip | Compute pipelines compile in the background and dispatches are skipped until ready (`sync` = on the calling thread; `wait` = background, a dispatch waits for its PSO) |
| `LowPriorityWorkers` | 1 | Compile threads run at below-normal priority |
| `LogLevel` | info | `perf` logs every placeholder |
| `Overlay` / `OverlayStats` | 1 / 1 | On-screen label / live counters in it |
| `OverlayOnlyWhenFocused` | 0 | Hide the label while the game isn't the foreground window |
| `OverlayOpacity` | 200 | 30–255 |
| `DebugCompileDelayMs` | 0 | Testing aid: delays every background compile so you can *see* skipped draws |

### Run logs (comparing on vs off)

Every launch that renders frames writes to `d3d12async-runs\` next to the DLL (or `%TEMP%` if that folder isn't writable):

- `<date>_<time>_dll-ON.csv` / `_dll-OFF.csv`: one row per second, with phase (startup or game), whether the startup cap was active, frames, fps, average / 99th-percentile / worst frame time, frames over 50 ms, PSOs created, background compiles, skipped draws, compile queue, forced waits, game CPU % and system CPU %.
- `<date>_<time>_dll-ON.txt`: the run summary, rewritten every 2 s: settings, when the startup precompile finished and how many PSOs it made, and gameplay average fps, 1% / 0.1% lows, worst frame and stutter counts (>50, >100, >250 ms). Frames during the startup precompile aren't recorded, since ON runs cap that stretch and OFF runs don't.
- `runs.txt`: every kept run's summary, newest first; the newest is marked `LATEST`.

For an OFF run set `Enabled=0`: the layer changes nothing and only measures. `RunLogsKeepOn` and `RunLogsKeepOff` set how many runs of each kind are kept; older ones are deleted when a new run starts.

`d3d12async.log` (next to the DLL, or in `%TEMP%` if that folder isn't writable) reports a stats line every 10 s: PSOs seen, placeholders handed out, draws skipped, failures.

## Trade-offs and limits

- **Pop-in instead of freezes.** An object whose PSO is still compiling is invisible for a few frames. Effects that accumulate over time (TAA/TSR history, some caches) can smear briefly when a pass is skipped.
- **Anti-cheat:** offline/single-player only. USPF won't install into games with anti-cheat unless you confirm.
- **Microsoft Store/Xbox installs** (`WindowsApps`) are write-protected and not supported.
- **Another `d3d12.dll` mod** (for example SCSKiller's optional *recorder*) can't be used at the same time. SCSKiller's pre-warming works alongside this, and is recommended: it removes most compiles before you launch.
- If a background compile **fails** (rare; the game would have received an error), that pipeline's draws stay skipped. The failure is logged.
- Graphics PSOs only. Ray-tracing state objects (`CreateStateObject`) are passed through untouched.

## Tested

`tests/pso_test.cpp` renders real triangles and reads the pixels back. 98 checks cover:
- the built-in defaults matching the shipped `d3d12async.ini`;
- placeholder hand-out, skipped draws, and correct drawing once the compile is ready;
- deep copy (the test overwrites its own descriptions right after the create call);
- the pipeline-stream path, compute passthrough, and failure handling in budget and late;
- private data, residency, `GetCachedBlob`, release mid-compile;
- initial-PSO command lists, bundles, 4-thread concurrency, all-threads mode, backpressure, and queue boosting;
- vtables copied by other layers, and driver-style private vtables whose entries bypass the runtime;
- the startup phase ending on its own once creations go quiet, and UI-type pipelines staying synchronous;
- the Present hook: the startup frame cap holding and lifting, and run logs with labelling and pruning;
- the startup phase holding for `StartupMinSeconds` on a cold launch, and ending early on a warm one;
- `UiWaitMaxMs` cutting a UI wait short;
- `QueueFullMode=placeholder` capped by `MaxPlaceholders`, and the settings panel's Ctrl+Shift+P toggle;
- loading screens: pipelines created while no frame is presented compiling normally, the hold after frames resume, and loading frames kept out of the gameplay stats;
- the experimental queue options: placeholders past the queue limit, the compile rate limit, and efficiency mode;
- the hitch tracker: a slow frame with no pipeline work and a frame blocked by a forced wait are each recorded with the right cause.

The suite runs under Wine with vkd3d and a software Vulkan driver. In real games on Windows with an RTX 4060:
- **STALKER 2:** about 3 minutes with no crashes. 10.5k graphics PSOs, of which 10.3k compiled in the background; 0 failures. No A/B against stock frame times yet.
- **Gears of War: E-Day:** crashed in 0.1.0 because the NVIDIA driver's private command-list vtables let a placeholder reach the driver. Fixed in 0.1.1; not yet retested in the game.

## Building

`./build.sh` (MinGW-w64; Linux or MSYS2) produces `build/d3d12.dll`. `./build.sh test` also builds `build/pso_test.exe`. Vtable slot numbers come from Microsoft's official C headers (`src/vtbl_index.c`), never hand-counted.

## License

MIT, © 2026 Meower. Uses Microsoft DirectX-Headers (MIT) at build time.
