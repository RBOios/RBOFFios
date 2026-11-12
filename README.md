# RBO port prototype (x86 interpreter + Win32/DirectX shim)

Goal: run Ragnarok Battle Offline (native x86 DirectDraw/Direct3D7 game) on non-Windows platforms, eventually iOS.

## Status (headless Linux test harness)
- PE32 loader, x86 interpreter (integer, string ops, x87), 143 imports resolved
- Win32 kernel/user shims, case-insensitive file mapping, guest heap, nested guest calls (WndProc, callbacks)
- DirectDraw7 (surfaces/blt/lock/flip), DirectInput8 (keyboard stub), DirectSound (buffer bookkeeping)
- rbo.exe boots through CRT init, opens all PAC archives, reaches display-mode setup
- Direct3D7 layer (software rasterizer) done: game renders CAUTION screen and logo fades correctly (see rbo_frames_contact_sheet.png)
- Verified end-to-end headless: logos -> title (Korean menu) -> Enter -> lobby scene with Korean dialogue (rbo_title_to_lobby.png)
- Env: RBO_FAST=1 deterministic virtual clock; RBO_KEYS="frame:dik_hex:down,..." scripted input
- NEXT: profile/speed (GPU backend, recompilation), audio out, gamepad, expansions 1-3 test, iOS front-end (menu: Original/Ex1/Ex2/Ex3)

## Build / run
    make && ./rboemu --root <game dir containing DATA/> --frames 5 --alarm 30 <game dir>/RBO.exe
Debug flags: --trace (file/API log), --trace2 (every HLE call), --alarm N (watchdog dump).
Env: RBO_DUMP_EVERY=N writes every Nth presented frame to /tmp/rbo_frames/*.ppm

## Checkpoint server (fast iteration)
    RBO_FAST=1 ./rboemu --server 1800 <game>/RBO.exe &      # pauses at frame 1800, listens on /tmp/rbo.cmd
    ./ctl.sh "script 5:1e:1,11:1e:0"   # relative-frame key events: frame:DIK_hex:down(1)/up(0)
    ./ctl.sh "go 600"                  # advance the checkpoint process itself
    ./ctl.sh "try 300 30 /tmp/try1"    # fork an experiment from this exact state, dump every 30 frames
    ./ctl.sh "dump /tmp/snap.ppm" / "clear" / "quit"

## Findings
- Shipped DATA/SaveData/KeyConfig.cdt: P1 = arrow keys (c8,d0,cb,cd) + A,S,D,F (1e,1f,20,21); A = confirm/action
- Shipped SystemConfig.cnf selects windowed mode (DDSCL_NORMAL): needs IDirectDrawClipper/Palette objects (implemented)
- With 2P/3P keys cleared (strip_p2p3_keys.py) the lobby counts 1 player and the flow reaches battle (rbo_battle.png)
- Verified path: Title -A-> players -> slot -> Load Game -> status -> skill -> Ready -> adventure -> Stage Select -A-> READY/GO battle; arrows move, A attacks
- Lobby flow (old, before key cleanup): Title -A-> player-count -> slot -> Load Game -> status -> skill -> Ready? -> waits for P2 ready (shared A key joins 2 players)

## Expansions
- RBO_Ex1/Ex2/Ex3.exe boot to their (Korean) title screens: Ex1 v2.011, Ex2 v3.000 (640x480 fullscreen path), Ex3 v4.000 (windowed 1280x1024, 2x scale)
- Needed extra APIs: registry (fail), SHBrowseForFolder/PathCombine/PathFileExists, CreateDialogParam, timeBeginPeriod
- Code range is read from the PE sections (not hard-coded); windowed mode uses a 1920x1200 fake desktop primary, dumps crop to the last Blt rect
- Env: RBO_FRAME_DIR=<dir> selects where frame dumps go

## Audio / threads (added)
- DirectSound software mixer (44.1 kHz stereo), clocked by the game clock; RBO_WAV=<file> captures the mix
- IDirectSoundNotify positions + cooperative threads (sched.c): BGM is streamed by 2 worker threads waiting on notify events
- Time-slice preemption every 131072 instructions; real critical sections, events, semaphores, WM_TIMER delivery
- DirectShow (dshow.c) is only a stub for the opening movie; RenderFile/Run are logged, the game continues without the movie
- BGM starts in the lobby (logos/title are silent)

## Clock / audio notes
- Fast mode (RBO_FAST=1) clock: guest time = max(frame clock 16.667 ms/frame, elapsed); timeGetTime ticks capped at 16 ms/frame; idle waits advance the base once (no double counting)
- RBO_DUMPSTREAM=<file> writes the raw PCM the game streams into its BGM buffer (22050 Hz, 2ch, 16bit) for offline checking
- The 0.667 s transients in the captured audio match a 90 BPM beat and are present in the raw stream before mixing; treated as music, not glitches (not verified by ear)
