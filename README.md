# AutoLabel

A lightweight desktop image annotation tool for building **YOLO segmentation
datasets** with Meta **SAM2**. Inspired by SegmentAnythingAnnotator, and built
to feel like a natural extension of **InferenceVisualizer** (same UI layout,
architecture, and Qt patterns).

- **Frontend:** C++17, Qt Widgets, OpenCV
- **Backend:** Python, PyTorch, SAM2 (a long-lived process spoken to over
  JSON via stdin/stdout)

See [`docs/index.html`](docs/index.html) for the living project doc
(architecture, per-phase changelog, and build instructions).

## Status

**Phase 1 — Project skeleton.** Qt-only `QMainWindow` with menu bar, status
bar, and a placeholder central widget. OpenCV / SAM2 arrive in later phases.

## Build (Windows, MinGW)

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
.\build\bin\AutoLabel.exe
```

CMake auto-detects the newest Qt 6.x under `C:\Qt`. Override with
`-DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\mingw_64` if needed.
