# RTSS Clone — All-in-One Overlay

One EXE. No setup. Drop it anywhere, run as Admin, done.

---

## Download

1. Go to **Actions** tab in this repo
2. Click latest green **Build RTSS Clone** run
3. Scroll to **Artifacts** → download **RTSS-Clone-AllInOne-x64.zip**
4. Extract — run **`RTSSClone.exe`** as Administrator

---

## What's inside RTSSClone.exe

`RTSSClone.exe` contains everything — `HookDLL.dll` is embedded inside
the EXE and extracted automatically to `%TEMP%` on launch.
You only need this one file.

| What | How |
|------|-----|
| Process list & injector | **INJECT tab** |
| Live overlay editor | **EDITOR tab** — position, colors, font, which rows show |
| Config saved automatically | `rtss_overlay.cfg` next to the EXE |

---

## How to use

1. Launch your game (e.g. Night Vision FNF mod)
2. Run `RTSSClone.exe` **as Administrator**
3. **INJECT tab** → find your game in the list → click **INJECT**
4. Overlay appears in-game within 2 seconds
5. Switch to **EDITOR tab** — all changes apply instantly, no restart needed

---

## Overlay style

Matches RivaTuner Statistics Server exactly:
- Orange labels, white values, grey units
- Transparent background — pure text on any game
- Frametime line graph at bottom
- FPS colour: white ≥60, orange ≥30, red <30

---

## What it shows

| Stat | Source |
|------|--------|
| FPS | `QueryPerformanceCounter` — real frames |
| Frame time ms | Same |
| CPU % | Windows PDH |
| RAM MB | `GlobalMemoryStatusEx` |
| GPU % | PDH GPU Engine (Intel iGPU compatible) |

No GPU temperature — Intel HD doesn't expose it without third-party tools.

---

## Files you can upload to GitHub to trigger a rebuild

| File | Path |
|------|------|
| `launcher.cpp` | `Launcher/launcher.cpp` |
| `hooks.cpp` | `HookDLL/hooks.cpp` |
| `dllmain.cpp` | `HookDLL/dllmain.cpp` |
| `CMakeLists.txt` | `CMakeLists.txt` |
| `build.yml` | `.github/workflows/build.yml` |

---

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| F5 | Refresh process list |
| ↑ ↓ | Navigate list |
| Ctrl+S | Save overlay config |
| Ctrl+O | Load overlay config |
| Right-click | Close |

---

## Anti-cheat warning

DLL injection is detected by anti-cheat (BattlEye, EAC, Vanguard).
**Only use with offline / single-player games.**
