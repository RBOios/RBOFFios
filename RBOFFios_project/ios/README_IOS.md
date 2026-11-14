# RBO for iOS (unverified scaffold)

**Status:** the C engine/API (`rbo_api.h`) is tested on a Linux host. Everything in `ios/RBOiOS/*.swift`,
`project.yml` and the workflow was written without Xcode or a device: it has **never been compiled or run**.
Expect small build errors on the first CI run.

## Build (unsigned IPA via GitHub Actions)
1. Upload the contents of this zip to a GitHub repository (the workflow is already at `.github/workflows/build-ios.yml`).
2. Run the "Build unsigned IPA" workflow; download `RBO-unsigned-ipa`.
3. Sideload with your usual tool (AltStore/SideStore/Sideloadly).
Locally: `cd ios && xcodegen generate && open RBOiOS.xcodeproj`.

## Using it
- Tap **게임 폴더 선택** and pick the folder containing `RBO.exe`, `RBO_Ex1~3.exe` and `DATA/` (copy it to the device's Files first).
- Pick 오리지널 / 확장팩 1~3. On launch the app clears 2P/3P keys in `DATA/SaveData/KeyConfig.*` (originals saved as `.bak`).
- Controls: physical gamepad (D-pad/left stick = arrows, A/B/X/Y = A/S/D/F keys) or the on-screen pad. In menus `A` confirms.

## Known limits
- The engine keeps global state: **one edition per app launch**. To switch edition, quit and relaunch the app.
- Rendering: 3D is drawn by a Metal rasterizer (`gpu_raster.m`), with the CPU rasterizer (`d3d.c`) as fallback (the GPU path is on by default and untested on a device). Put an empty `rbo_gpu_off.txt` in the game folder to force the CPU path, or `rbo_gpu_auto.txt` to switch automatically by scene size. Check the `[GPU]` lines in the perf log (`rbo_perf`).
- Guest memory is a 512 MB `mmap` (lazily committed); not tested against iOS memory limits.
- Movies (`DATA/Movie/*.mpg`, MPEG-1 + MP2) are decoded in software with [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (`pl_mpeg.h`, MIT) because iOS has no MPEG-1/MP2 decoder. `dshow.c` emulates the DirectShow graph the game uses (IMediaControl/IMediaEventEx/IMediaSeeking/IMediaPosition/IBasicVideo/IBasicAudio/IVideoWindow); frames go to the Metal view, audio is mixed into the same ring buffer as the game's sound, and EC_COMPLETE is delivered through the window message the game registered. If the file is missing or not MPEG-1 the game reports the failure and continues without a movie.
- Real-time audio is pulled from a ring buffer; underruns produce silence/glitches if emulation falls below real time.
