# RTSS Clone — In-Game Overlay

MSI Afterburner / RTSS style overlay for Windows games.
Shows FPS, frame time, CPU, RAM, and GPU usage inside the game.

---

## What You Need to Upload to GitHub

Every time you get a new ZIP, upload **only these files** (replace the old ones):

| File | Location in repo |
|------|-----------------|
| `hooks.cpp` | `HookDLL/hooks.cpp` |
| `dllmain.cpp` | `HookDLL/dllmain.cpp` |
| `CMakeLists.txt` | `CMakeLists.txt` |
| `build.yml` | `.github/workflows/build.yml` |

> **Tip:** When uploading on GitHub.com, go to the file → click pencil icon → paste contents → commit.
> For `.github/workflows/build.yml`, type the path manually in "Create new file" — drag and drop skips hidden folders.

---

## How to Download the Built EXE and DLL

1. Go to your repo on GitHub
2. Click the **Actions** tab
3. Click the latest green **Build RTSS Clone** run
4. Scroll to **Artifacts** at the bottom
5. Download **RTSS-Clone-Binaries-x64.zip**
6. Extract — you get:
   - `Injector.exe`
   - `HookDLL.dll`

---

## How to Use

### Requirements
- Windows 10 version 1803 or later
- Run as **Administrator**

### Steps
1. Launch your game (e.g. Night Vision FNF mod)
2. Run `Injector.exe` **as Administrator**
3. A list of running processes appears — find your game and enter its PID
4. The overlay appears in the **top-left corner** of the game within 2 seconds

That's it. No other tools needed.

> **SystemStats.exe** is no longer needed — stats are collected inside the game process automatically.

---

## What the Overlay Shows

- **FPS** — real frame count via QueryPerformanceCounter (yellow/orange/red)
- **Frame time** — milliseconds per frame
- **CPU usage** — total processor load %
- **RAM usage** — used / total GB
- **GPU usage** — Intel HD iGPU load % via Windows PDH
- **GPU mem** — shared memory usage (iGPU shares system RAM)
- **Frametime graph** — last 26 frames, red bars = below 60fps

---

## Supported APIs

| API | Game Engine | Status |
|-----|-------------|--------|
| OpenGL | Psych Engine, OpenFL, Lime | ✅ Full overlay |
| DirectX 11 | Most modern games | ✅ Full overlay |
| Vulkan | Some games | ✅ FPS count only |
| DirectX 9 | Older games | ✅ FPS count only |

---

## Why No GPU Temperature?

Intel HD Graphics does not expose temperature through any public Windows API.
It requires third-party driver access (like HWiNFO or OpenHardwareMonitor).
The temp row is hidden to keep the overlay clean.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "Injection failed" | Run Injector.exe as Administrator |
| Overlay doesn't appear | Wait 3 seconds — hook needs time to init |
| FPS shows 0 | Game may use an unsupported renderer |
| Game crashes on inject | Try injecting after the game's main menu loads |
| CPU/RAM/GPU show 0 | Stats start ~1 second after overlay appears |

---

## Anti-Cheat Warning

DLL injection is detected by anti-cheat systems (BattlEye, EAC, Vanguard).
**Only use with offline / single-player games.**
