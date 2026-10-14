# d3d12async: features and behaviour

This file explains what the DLL does while a game runs, feature by feature, and which
setting in `d3d12async.ini` controls each one. For installing, see `README.md`.

---------------------------------------------------------------------------------------
## 1. What happens during one launch

1. **The game starts and loads `d3d12.dll`.** The DLL reads `d3d12async.ini` from its own
   folder (missing keys use the defaults below), opens `d3d12async.log`, and forwards
   every D3D12 call to the real `C:\Windows\System32\d3d12.dll`.
2. **The game creates its D3D12 device.** The DLL attaches its hooks (in memory only), starts
   its background compile threads, finds the swap chain's `Present` call, and shows the
   on-screen label.
3. **Startup phase.** While the game runs its initial shader/pipeline precompile, the DLL
   changes nothing about compiling: every pipeline compiles the normal way, nothing is
   skipped. It only:
   - caps the frame rate (30 fps by default) so rendering leaves more CPU to compiling;
   - gives the game thread doing a compile the highest thread priority for that compile.

   Label: `d3d12async startup: compiling normally, 30 fps cap`.
4. **The precompile ends.** The DLL notices from the game's own pipeline activity (not a
   timer) and switches on background compiling, without a restart. The frame cap lifts at
   the same moment. Label: `d3d12async active`. The log records the time.
5. **Gameplay.** A pipeline the game needs that isn't compiled yet compiles in the
   background. Until it's ready, the few objects that need it are not drawn for a few
   frames, instead of the whole game freezing.
6. **Throughout the run**, frame times and pipeline activity are written to the run logs.

---------------------------------------------------------------------------------------
## 2. Features

### Background compiling (the main feature)
- **What:** when the game asks for a new pipeline (PSO) during gameplay, the compile is
  queued on the DLL's own threads. If it finishes within `BudgetMs` (1 ms; driver-cache
  hits take well under 1 ms), the game gets the real pipeline as usual. Otherwise the game
  gets a **placeholder** right away and carries on.
- **While a placeholder is in use**, draw calls that use it are skipped. The moment the
  real pipeline is ready it's used everywhere the placeholder was.
- **Queue boost:** a placeholder the game actually tries to draw with jumps to the front of
  the compile queue.
- **Backpressure:** once `MaxQueuedCompiles` compiles are waiting, threads that don't record
  command lists (loading / precache threads) compile the normal way again, so a flood can't
  pile up memory.
- **Visible effect:** brief pop-in of individual objects instead of a frame freeze.
- **Settings:** `Enabled`, `BudgetMs`, `AsyncThreads`, `MaxQueuedCompiles`.

### Things that are never deferred (always compiled the normal way)
- **Everything during the startup phase** (section 3).
- **UI-type pipelines are never skipped:** those that only draw to 8-bit colour targets with
  no depth buffer (menus, text, HUD, cached widget images). UI is often drawn once into a
  cached image and reused, so a skipped draw there would stay empty (the empty Borderlands 4
  settings menu). `UiPipelineMode`:
  - `wait` (default): compiled in the background like the rest, but a draw that needs one
    waits for it, compiling it on the drawing thread if no worker has started it. A menu
    that brings 70 new UI pipelines at once compiles them in parallel instead of one by one
    (that one-by-one stall was a 507 ms Townfall hitch).
  - `sync`: compiled on the calling thread, one at a time (older behaviour).
  - `async`: like any other pipeline; draws are skipped until ready.
  - `UiWaitMaxMs` (default 2; 0 = off): with `wait`, a draw waits at most this long, then is skipped
    like any other. In Borderlands 4 the waits were ~110 ms each when a menu or HUD screen
    opened for the first time. The risk is the empty-widget problem above, for slow ones only.
- **CPU-relief options** (`QueueFullMode=placeholder` and `WorkerEfficiencyMode=1` are on by
  default since 0.1.34, the settings from the best Borderlands 4 run; `MaxCompilesPerSecond` is off):
  - `QueueFullMode=placeholder`: once `MaxQueuedCompiles` compiles are waiting, new creates
    still get placeholders, but only while fewer than `MaxPlaceholders` (512) placeholders are
    outstanding (not compiled yet); past that they compile normally. The two limits are
    separate: `MaxQueuedCompiles` is the normal background queue, `MaxPlaceholders` caps the
    extra placeholders. Gameplay bursts stay under it (Borderlands 4 peaked at ~430), while a
    loading screen or precache flood of thousands mostly compiles normally, so the screen
    waits for its shaders. Without the cap (earlier versions: 8192) a loading screen could
    finish early and leave its shaders to compile in gameplay. Toggle live with Ctrl+Shift+P.
  - `WorkerEfficiencyMode=1`: compile threads run in Windows 11 efficiency mode (EcoQoS: the
    scheduler prefers efficiency cores and lower clocks) with low memory priority. CPU
    priority stays below-normal rather than idle, so the workers can't be starved outright.
  - `MaxCompilesPerSecond=N`: workers start at most N queued compiles per second, spreading
    the same work over more time. Compiles a game thread is waiting on are exempt. Pair it
    with `QueueFullMode=placeholder`, or the queue fills sooner and more creates fall back to
    the game's thread.
  - A single compile happens inside the driver in one call, so none of these make one compile
    cheaper; they change when and where the work runs. The run summary shows the longest
    gameplay queue, placeholders handed out past the limit, and time spent rate-limited.
- **Compute pipelines** (`ComputePipelineMode`, default `skip`). Skipping compute work (culling,
  Nanite, Lumen, particles) can cause glitches; none were seen in Borderlands 4:
  - `sync`: compiled on the calling thread, as the game does on its own, usually
    on many task threads in parallel.
  - `skip` (default): compiled in the background, and dispatches that use one are
    skipped until it's ready, exactly like draws. Nothing ever waits, but the skipped compute
    work is missing for those frames: expect flicker, missing particles or effects, wrong
    lighting, possibly worse. The run summary counts the skipped dispatches.
  - `wait`: compiled in the background; a dispatch that needs one waits for it, compiling it
    on that thread if no worker has started it. Only helps games that precache compute well
    ahead of use. In Townfall it made level loads much worse (8 s stalls): the game needed
    hundreds of them at once, and the render thread ended up compiling them one by one.
- **Bundles**, and anything that needs the real object: residency calls, `GetCachedBlob`,
  unknown interfaces. These wait for the real pipeline (counted as "forced waits").
- **Pipeline-cache stores:** when Unreal stores a still-compiling pipeline in its cache
  library, the call returns at once and the real pipeline is stored when its compile
  finishes. A cache lookup for it in the meantime misses, which Unreal already handles.

### Startup-phase detection
- The phase is active from launch. It ends once the DLL has seen at least
  `StartupMinPsos` (64) pipeline creates/loads, followed by `StartupQuietSeconds` (10)
  quiet seconds in a row.
- **A second is busy** (and restarts the quiet count) if either:
  - the game made more than `StartupQuietRate` (10) pipeline creates/loads in it, or
  - the game's CPU use was above `StartupCpuBusyPercent` (80% of all CPUs). A precompile
    keeps the CPU near 100%, so the phase can't end while the game is still compiling,
    even through routes the DLL can't see.
- The quiet count starts from the last busy second (or launch). Time before the game
  started working never counts as quiet.
- **The precompile must have been seen first:** the phase can't end until there have been
  `StartupFloodSeconds` (10) busy seconds after the game's first frame. Borderlands 4 plays
  about 20 quiet seconds of intro between engine start-up and its compile screen, which used
  to end the phase before the compile screen began. If no precompile starts within
  `StartupFloodWaitSeconds` (120) of the first frame (caches already warm), the normal quiet
  rule applies anyway.
- **Minimum time:** for `StartupMinSeconds` (150) after launch nothing but the hotkey ends the
  phase; after that, normal detection carries on. In Borderlands 4 the intro made 2,664
  pipelines, then the game went quiet for 17 s before its compile screen, which once ended the
  phase too early: the 50k-pipeline precompile then got placeholders and the compile screen was
  skipped.
- The experimental options (`QueueFullMode`, `WorkerEfficiencyMode`, `MaxCompilesPerSecond`)
  never apply during the startup phase.
- **By hand:** Ctrl+Shift+End ends the phase immediately (`StartupHotkey=1`), for a game that
  keeps compiling into gameplay so the phase never ends on its own. The label shows the hint
  while the phase is active.
- **Safety net:** `StartupMaxSeconds` (0 = off) ends the phase after that many seconds no
  matter what. Useful only for a game whose CPU use never drops below the threshold.
- **Counted as activity:** graphics and compute pipeline creation, and loads from the
  game's pipeline-cache library (a load can compile inside the driver).
- **Checked:** at most 4 times a second, from the game's own frame submissions. It costs
  nothing measurable.
- **Tuning:** if the label switches to "active" while the game is still on its compile
  screen, raise `StartupQuietSeconds`. If it never leaves "startup", lower it or raise
  `StartupQuietRate`.
- Detection also runs with `Enabled=0`, so OFF run logs show when the precompile ended.
- Settings: `StartupCompileNormally`, `StartupMinPsos`, `StartupQuietSeconds`,
  `StartupQuietRate`, `StartupCpuBusyPercent`, `StartupMaxSeconds`.

### Loading screens (experimental, off by default)

Pipelines created while the game shows no new frame for `LoadingGapMs` (500 ms) compile normally,
as without the DLL. In gameplay a frame comes every few ms and, with background compiling, a
pipeline create never holds one up, so a stretch with no frames while pipelines are being created
is a loading screen (Borderlands 4: 7–9 s without a frame while ~2,700 pipelines were created). The
loading screen then waits for its shaders instead of finishing early and leaving them to compile
in gameplay with missing objects. After a gap of at least 2x `LoadingGapMs`, normal compiling
continues for `LoadingHoldMs` (1.5 s) after frames resume, counted from the first new frame.

- Re-entrant, unlike the startup phase: every loading screen is detected. No frame cap, no
  priority changes beyond the compiling thread's boost.
- A gameplay hitch that already lasts 500 ms also counts: pipelines created during it compile
  normally (they can lengthen it). Gameplay hitches that long were rare in the logs.
- A loading screen that keeps presenting frames (an animated one) isn't detected; its pipelines
  get placeholders as before.
- If no frame is seen for over 60 s, the Present hook has most likely lost track of the frames
  (another layer replaced the swap chain): detection then treats it as gameplay and logs a warning.
- Loading-screen frames (long, with normal compiles in them) are left out of the gameplay frame
  stats and hitches. The run summary has a `loading` line: how many, how long, how many pipelines.
- **Off by default** (`LoadingCompileNormally=1` turns it on): how a loading screen behaves varies too much between games.

### Startup frame cap
- **When:** only while the startup phase is active **and** the game is busy (a busy second
  within the last 2 s). It's lifted during the quiet countdown at the end of the phase,
  when the phase ends, and never applies during gameplay.
- **How:** in the game's `Present` call: a high-resolution sleep, then a short spin for
  accuracy.
- **Why:** a compile screen rendered at hundreds of fps keeps a core or two busy for
  nothing. Capped, that CPU goes to compiling. The gain is real only when the CPU is
  close to 100% (the run log shows this).
- Setting: `StartupFpsCap` (30; 0 = off).

### Startup compile-thread priority boost
- **What:** during the startup phase, each game thread that is compiling a pipeline runs at
  the highest thread priority for exactly that call, then drops back to its own priority.
- **Why:** compiling gets the CPU before the rest of the game.
- Setting: `StartupBoostCompileThreads`.

### Background compile threads
- **Count:** `Workers` (2; 0 = a quarter of your hardware threads, clamped to 2–8).
- **Priority:** below normal (`LowPriorityWorkers=1`), so they use spare CPU time first.
- **CPUs:** `WorkerCpus`, like Task Manager's "Set affinity". `upper-half` (default) puts
  them on the last half of your CPUs (CPU 8–15 on a 16-thread CPU), away from the cores
  games usually load first. Only these threads are restricted; the game's own threads are
  untouched.
- **Burst workers:** `BurstWorkers` extra threads (auto = as many as `WorkerCpus` allows,
  max 8) join only while more than `BurstQueue` (16) compiles are waiting, e.g. a level
  load or precache wave, then go idle again. Same priority and CPUs as the others.
- **Queue limit:** `MaxQueuedCompiles` (256). Past it, threads that don't record command
  lists compile normally again, in parallel on the game's own threads, so a level-load wave
  doesn't pile up behind a few workers.
- They are idle during the startup phase, because compiling then happens on the game's
  own threads.

### Settings panel (bottom left)

A second small click-through window in the bottom left of the game window shows settings that
can be changed live, with hotkeys while the game has focus (a click would take focus and the
mouse away from the game). For now: **Ctrl+Shift+P** turns queue-full placeholders
(`QueueFullMode`) on and off, and the panel shows the current state, `MaxPlaceholders` and how
many placeholders are outstanding. The toggle lasts until the game closes (the ini is not
changed) and is written to `d3d12async.log`. `SettingsPanel=0` (the default since 0.1.34) hides it, but Ctrl+Shift+P still works; `Overlay=0` hides both and turns the hotkey off.

### On-screen label
- A small click-through window on the left of the game window, separate from the game's
  frames. Visible in windowed and borderless mode, not in exclusive fullscreen; hidden
  while the game isn't the foreground window.
- **States:**
  - `d3d12async startup: compiling normally, 30 fps cap`: startup phase, cap holding
  - `d3d12async startup: compiling normally`: startup phase, cap off or not needed
  - `d3d12async active`: background compiling is on
  - `d3d12async off (Enabled=0), measuring`: DLL loaded but changing nothing
- **Live line:** compiles waiting in the background queue, and for the last second: new
  pipelines, pipelines sent to the background, ms the game's threads spent blocked in
  pipeline calls, draws skipped.
- **Hitch line:** number of gameplay hitches (frames of `HitchMs`, 50 ms, or slower) and how
  many came from a pipeline compile. Then the most recent one: how long it was, how long ago,
  and its cause. Red for 5 s after a compile hitch, amber after any other hitch.
- **Frame-time graph:** the last ~130 frames. Green = normal, amber = hitch without a
  pipeline compile, red = hitch caused by one. The dotted line is `HitchMs`.
- Settings: `Overlay`, `OverlayStats`, `OverlayGraph`, `OverlayOnlyWhenFocused`,
  `OverlayOpacity`, `HitchMs`.

### Hitch tracker
- Every pipeline call on a game thread is timed and tagged with why it ran the way it did:
  - background compile (waited at most `BudgetMs`);
  - compiled normally: startup phase, UI-type pipeline, compute pipeline, compile queue
    full, or not eligible for background compiling;
  - pipeline-cache load;
  - forced wait for a background compile.
- Each frame (one `Present`) collects the calls that finished during it. A gameplay frame
  slower than `HitchMs` is a **hitch**, classified as:
  - **pipeline compile**: one call took at least half the frame. Its cause and thread
    (render / background) are shown, e.g. "UI-type pipeline, compiled normally: 180 ms on
    a render thread";
  - **forced wait**: the game needed the real object of a pipeline still compiling;
  - **several pipeline calls**: together they took at least half the frame;
  - **no pipeline compile**: nothing pipeline-related blocked that frame. The hitch is
    streaming, other CPU work, or the GPU, so the DLL can't fix it.
- Compile and forced-wait hitches are also written to `d3d12async.log` (first 500).
- A hitch's cause is the most likely one, not proof. A long compile on a background
  thread during a slow frame doesn't always mean it caused that frame.

### Run logs (comparing ON vs OFF)
- **Where:** `d3d12async-runs\` next to the DLL, or `%TEMP%\d3d12async-runs` if the game
  folder isn't writable.
- **When:** a run starts at the game's first frame. Processes that never render (launchers,
  probes) create no files.
- **Files per run** (name = start date and time + label):
  - `YYYY-MM-DD_HH-MM-SS_dll-ON.csv`: one row per second, with these columns:
    - phase (startup / game), whether the frame cap was active;
    - frames, fps, average / 99th-percentile / worst frame time, frames over 50 ms;
    - graphics pipelines created, compute pipelines created, library loads;
    - ms the game spent inside pipeline calls (time its threads were blocked compiling);
    - placeholders, skipped draws, compile queue, forced waits;
    - game CPU %, whole-system CPU %, disk read MB/s, GPU 3D-engine %;
    - fastest frame, 99.9th-percentile frame time, and that second's 1% and 0.1% lows.
  - `YYYY-MM-DD_HH-MM-SS_dll-ON_frames.csv`: every frame: seconds after launch, frame time,
    and 1 for gameplay or 0 for the startup precompile / a loading screen. USPF's frame-time
    chart reads it.
  - `YYYY-MM-DD_HH-MM-SS_dll-ON_hitches.csv`: one row per gameplay hitch: time, frame
    ms, cause, description, longest pipeline call and its thread, pipeline calls and their
    total ms in that frame, new placeholders, skipped draws, forced waits, queue.
  - `YYYY-MM-DD_HH-MM-SS_dll-ON.txt`: summary, rewritten every 2 s so it's complete even
    if the game is killed:
    - settings used, when the precompile finished and how much pipeline work it did;
    - gameplay frame stats: average fps, 1% and 0.1% lows, worst frame, and frames over
      50 / 100 / 250 ms;
    - max fps (best one-second average in gameplay) and the fastest single frame;
    - hitches by cause, the 5 worst with their cause, and game-thread time spent in
      pipeline calls during gameplay, by cause.
  - **Frames during the startup precompile are not recorded** (the frame columns of those
    csv rows are empty). An ON run caps that stretch at 30 fps and an OFF run doesn't, so
    including it would make the comparison meaningless. Compare the precompile by when it
    finished instead.
- **`runs.txt`:** all kept runs' summaries, newest first; the newest is marked `LATEST`.
- **ON / OFF:** `dll-ON` = layer active. `dll-OFF` = a launch with `Enabled=0`. The DLL
  then changes nothing and only measures (`MeasureWhenDisabled=1`), so it's a fair
  baseline. Deleting the DLL gives no logs at all.
- **Retention:** `RunLogsKeepOn` / `RunLogsKeepOff` runs of each kind are kept (1 each by
  default); older ones are deleted when a new run starts.
- Settings: `RunLogs`, `RunLogsKeepOn`, `RunLogsKeepOff`, `MeasureWhenDisabled`.

### Diagnostic log and crash log (`d3d12async.log`)
- **Startup lines:** settings, devices hooked, compile pool, present hook.
- **Events:** when the startup precompile finished; a stats line every 10 s once background
  compiling has started; compiles slower than `SlowCompileLogMs`.
- **Forced waits:** each one with the reason and the module that caused it.
- **Crashes:** if the game crashes, the exception and a stack with module names are
  written here too, so you can tell whether the DLL was involved.
- Settings: `LogLevel` (`perf` logs every placeholder; for diagnosis only),
  `SlowCompileLogMs`.

---------------------------------------------------------------------------------------
## 3. Guarantees: what the DLL never does

- **On disk:** it never modifies the game's files, the driver or Windows files. All hooks
  are in memory, on the game's own D3D12 objects, and disappear when the game closes.
  Removing the DLL uninstalls it completely.
- **Anti-cheat / DRM:** it never bypasses either. USPF refuses games with
  anti-cheat unless you confirm; only use it offline / single-player.
- **Startup:** it never skips draws during the startup phase, and never skips UI-type or
  compute work.
- **Failed compiles:** it never hands the game a broken pipeline. If a background compile
  fails, that pipeline's draws stay skipped and the failure is logged.
- **Unexpected drivers:** if the driver's internal objects behave in a way the DLL can't
  safely track, it switches placeholders off for the rest of the session instead of risking
  a crash.
- **Caches:** it never deletes shader caches on its own. USPF's cache clearing
  lists everything first and needs you to type `YES`.

---------------------------------------------------------------------------------------
## 4. Troubleshooting by symptom

| Symptom | Likely cause | What to change |
|---|---|---|
| Menu or HUD text missing | A UI pipeline was skipped | Make sure `UiPipelineMode` isn't `async`; send the log if it persists |
| Label shows "active" while the compile screen is still up | Precompile paused longer than the quiet window | Raise `StartupQuietSeconds` (e.g. 20) |
| Label stays on "startup" in gameplay (30 fps) | The game keeps creating many pipelines per second, or its CPU use stays above `StartupCpuBusyPercent` | Raise `StartupQuietRate` or `StartupCpuBusyPercent`, set `StartupMaxSeconds`, or set `StartupFpsCap=0` |
| Lots of pop-in | Compiles finish slowly | More `Workers`, `WorkerCpus=all`, or raise your GPU driver's shader cache size |
| Still freezes | Forced waits, or compiles outside the DLL's reach | Check "forced waits" in the log and `ms_in_pso_calls` in the .csv |
| No run logs | `Present` hook didn't attach | Look for `present hook` in `d3d12async.log` |
| Crash on launch | Conflict with another DLL or overlay | Set `Enabled=0`; if it still crashes, remove the DLL and send the log |

---------------------------------------------------------------------------------------
## 5. All settings (`d3d12async.ini`, section `[d3d12async]`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | 0 = change nothing (still measures if `MeasureWhenDisabled=1`) |
| `BudgetMs` | 1 | How long a create may block before a placeholder is handed out; skipped when compiles are already queued |
| `AsyncThreads` | all | `all` (needed for UE5) or `recording` (only threads that record command lists) |
| `MaxQueuedCompiles` | 256 | Backpressure limit for non-recording threads |
| `QueueFullMode` | placeholder | Past `MaxQueuedCompiles`: `compile` = on the game's thread; `placeholder` = still a placeholder, waits its turn (safety cap 8192) |
| `MaxPlaceholders` | 512 | With `QueueFullMode=placeholder`: at most this many placeholders outstanding past the queue limit (max 8192) |
| `SettingsPanel` | 0 | Settings panel in the bottom left; Ctrl+Shift+P toggles queue-full placeholders live |
| `WorkerEfficiencyMode` | 1 | 1 = compile threads in Windows 11 efficiency mode (EcoQoS) with low memory priority |
| `MaxCompilesPerSecond` | 0 | **Experimental.** Start at most this many background compiles per second; urgent ones (a draw is waiting) are exempt. 0 = no limit |
| `ComputePipelineMode` | skip | Compute pipelines: `skip` = background, dispatches skipped until ready; `sync` = compile on the calling thread; `wait` = background, a dispatch waits |
| `StartupCompileNormally` | 0 | Compile normally during the startup precompile (off by default since 0.3) |
| `StartupMinPsos` | 64 | Pipeline creates/loads needed before the phase can end |
| `StartupQuietSeconds` | 10 | Quiet seconds in a row that end the phase (1–60) |
| `StartupQuietRate` | 10 | Max pipeline creates/loads per second that still count as quiet |
| `StartupCpuBusyPercent` | 80 | Game CPU % (of all CPUs) above which a second counts as busy; 0 = ignore CPU |
| `StartupFloodSeconds` | 10 | Busy seconds after the first frame needed before the phase may end; 0 = don't wait for a precompile |
| `StartupFloodWaitSeconds` | 120 | If no precompile starts within this long after the first frame, the normal quiet rule applies |
| `StartupHotkey` | 1 | Ctrl+Shift+End ends the startup phase by hand |
| `LoadingCompileNormally` | 0 | Experimental, off by default: compile normally while no frame is presented (loading screens) |
| `LoadingGapMs` | 500 | No frame for this long = loading screen |
| `LoadingHoldMs` | 1500 | Keep compiling normally this long after frames resume (after a gap of 2x `LoadingGapMs`) |
| `StartupMinSeconds` | 150 | For this long after launch only the hotkey can end the startup phase; 0 = off |
| `StartupMaxSeconds` | 0 | Hard limit on the startup phase; 0 = none |
| `StartupBoostCompileThreads` | 1 | Highest priority for the compiling game thread during startup |
| `StartupFpsCap` | 30 | Frame cap during the startup phase only; 0 = off |
| `UiWaitMaxMs` | 2 | With `UiPipelineMode=wait`: a draw waits at most this long for its UI pipeline, then is skipped; 0 = wait for the whole compile |
| `UiPipelineMode` | wait | UI-type pipelines: `wait` (background, draws wait, never skipped), `sync`, or `async` |
| `Workers` | 2 | Background compile threads; 0 = auto (2–8) |
| `BurstWorkers` | auto | Extra compile threads while more than `BurstQueue` compiles wait; auto = CPUs allowed by `WorkerCpus` (max 8), 0 = none |
| `BurstQueue` | 16 | Queue length at which burst workers join |
| `LowPriorityWorkers` | 1 | Compile threads at below-normal priority |
| `WorkerCpus` | upper-half | `upper-half`, `all`, or a list like `8-15` / `8,10,12-15` |
| `Overlay` | 1 | On-screen label |
| `OverlayStats` | 1 | Counters line in the label |
| `OverlayOnlyWhenFocused` | 0 | Hide the label when the game isn't in front |
| `OverlayOpacity` | 200 | 30–255 |
| `OverlayGraph` | 1 | Frame-time graph in the label |
| `HitchMs` | 50 | Gameplay frames at least this slow are recorded as hitches; 0 = off |
| `RunLogs` | 1 | Per-run frame-time logs |
| `RunLogsKeepOn` | 1 | ON runs to keep |
| `RunLogsKeepOff` | 1 | OFF runs to keep |
| `MeasureWhenDisabled` | 1 | With `Enabled=0`, still hook (pass-through) and log |
| `LogLevel` | info | error, warn, info, perf, debug |
| `SlowCompileLogMs` | 50 | Log background compiles slower than this |
| `DebugCompileDelayMs` | 0 | Testing only: delay every background compile |
