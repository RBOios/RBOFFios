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
- Rendering is currently the CPU rasterizer; Metal only displays the finished frame. Heavy scenes (esp. Ex3 at 1280x1024) may not hold 60 fps on every device. A GPU path would replace `d3d.c`'s rasterizer.
- Guest memory is a 512 MB `mmap` (lazily committed); not tested against iOS memory limits.
- The opening movie (DirectShow) is not supported; the game shows no movie and continues.
- Real-time audio is pulled from a ring buffer; underruns produce silence/glitches if emulation falls below real time.
