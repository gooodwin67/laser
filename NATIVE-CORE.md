# Laser Core (Windows) — experimental branch

This branch is **independent of the working GitHub Pages detector**.
**Do not merge into main yet.** The existing `index.html` on main and the published
`https://gooodwin67.github.io/laser/` remain unchanged.

## Two separate camera applications

The old browser Laser Lab on `main` stays unchanged. This experimental branch
contains two new camera applications and a separate Three.js test game.

### Windows native: `laser-core.exe`

The application shows the camera over the available window area, with a compact
top toolbar:

- **Camera / Mask** switches between the camera picture and detector mask
- **Presets** provides three trial detector configurations and separate **Save**
  / **Load User Preset** buttons
- **Settings** opens brightness, red excess, temporal change, spot size, focus,
  exposure, autofocus and autoexposure controls with short descriptions. It also
  offers native capture resolution and requested FPS selectors; the driver may
  refuse unsupported modes.
- **Calibration**: click **Select four corners**, then click the image corners
  **top-left → top-right → bottom-right → bottom-left**, as visible in the
  camera image. They map to the corners of the projected game screen.
- **Zeroing**: select **Shoot, then click target**, fire once, and click on the
  camera image at the point you *intended* to hit. This calculates X/Y offsets.
  Two sliders allow manual adjustments afterward.

Windows preferences and calibration are kept in `laser-core.ini` beside the
EXE. Keep this file if replacing the executable. Presets are experimental
starting points, not camera-specific guarantees.

Until four-point calibration is complete, the native app sends raw normalized
camera coordinates. After calibration it sends game-screen coordinates,
including X/Y zeroing offsets, to `ws://127.0.0.1:8765`.

### Browser: `web-detector/index.html`

Download and open in Chrome/Edge, or serve from a secure HTTPS origin.
The browser needs camera permission. Everything related to the camera is
handled by browser APIs (no native EXE required).

The browser camera fills the available screen, and its toolbar provides
Presets, Settings, Calibration, Zeroing and a mask preview. Personal settings,
calibration and presets are saved in that browser's localStorage.

The browser detector has its own coordinate mapping and spot detection. It
currently runs standalone as a diagnostic app; the native WebSocket output to
`game/index.html` belongs to the Windows variant. It does not automatically
send shots to the Three.js test game yet.

Both versions use four source points (as seen through the camera) mapped to
the normalized corners of the projected screen, with offsets applied afterward.
The four points require a visible, stable projected rectangle. Neither version
can recover a laser pulse that did not appear in any captured camera frame.

## Components

- `core/main.cpp` — native Windows USB camera capture with DirectShow + OpenCV,
  red laser spot detection, shot events via a local WebSocket server.
- `core/CMakeLists.txt` — CMake project.
- `core/vcpkg.json` — OpenCV dependency.
- `game/index.html` — Three.js moving sphere demo, consuming normalized `x`/`y` shot events.

This is **a minimal prototype, not yet a reliable shooting-range application**.
It has no projector calibration, camera parameter UI, or pulse-detection guarantees.
For this first test, point the camera squarely at the display and use its entire
field of view, or simply test via the mouse. Camera coordinates are not yet
corrected for projector geometry.

## Build (Windows 10/11, Visual Studio 2022 C++ tools)

Install Git, CMake, Visual Studio C++ Desktop Development tools and vcpkg.
Set the `VCPKG_ROOT` environment variable to the vcpkg checkout.

From the repository root, in a Developer PowerShell:

```powershell
cmake -S core -B build/native -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build/native --config Release
.\build\native\Release\laser-core.exe 0 640 480 30 190 45
```

Arguments: camera index, capture width, capture height, requested FPS,
minimum red brightness and minimum red excess over green/blue.

Open `game/index.html` in a modern browser. For ES modules, serving the game
from a local HTTP server is more reliable than file://. For example, from
the repository root:

```powershell
py -m http.server 8080 --directory game
```

Then visit `http://127.0.0.1:8080`. The first demo fetches Three.js from a
CDN; an offline copy will be bundled later. The WebSocket connection itself
is local and uses `ws://127.0.0.1:8765`.

## Native camera preview and controls

The latest native build opens a Windows preview with the camera on the left and
the detection mask on the right. Beneath them you can change:

- **Brightness** (0–255): minimum required red-channel brightness
- **Red excess**: how much red must exceed green and blue
- **Red change**: required increase compared with the immediately previous frame
- **Spot pixels**: minimum connected laser spot area, default 4 (to reject isolated noise)
- **Focus**: manual focus position (uncheck **Auto focus** to apply)
- **Exposure**: DirectShow exposure setting, typically negative log2 values
  (uncheck **Auto exposure** to apply)

The application displays driver-reported exposure, focus and FPS. The sliders
are experimental: if the USB camera or DirectShow driver does not support a
particular control, it may ignore the value. Full camera format selection is
not yet available in the UI: restart the EXE with the desired width, height
and requested FPS arguments to change the capture mode.

A short physical laser pulse entirely between camera exposures cannot be
recovered by software. The preview is for diagnostics and can modestly
reduce capture performance.

## What to test first

1. Start the executable; confirm the actual video mode and captured FPS printed
   in its terminal.
2. In the camera preview, verify a laser flash makes a green circle and a
   "Shot" line appears. Keep the laser away from the camera lens, eyes and
   reflective surfaces.
3. Open the Three.js demo. Verify the connection status and use mouse clicks to
   test the moving sphere without a laser.
4. Only then test real shot events and compare the camera's actual captured
   FPS to our former browser detector.

The WebSocket server currently supports a single local game client. The native
preview remains visible for diagnostics and may cost performance. Brief pulses
shorter than the physical exposure timing can still be missed regardless of
software speed. The detector emits an event only on a new detected spot
(with a 150 ms minimum event interval).

## Next steps

- Native camera controls: supported formats, manual exposure, focus and FPS.
- Projector 4-corner homography calibration.
- Frame timestamps, latency measurements and more robust pulse detection.
- Bundle local Three.js assets and an installer for an offline club setup.
