# AutoLabel

A lightweight desktop tool for building **YOLO datasets** — both **object detection**
(bounding boxes) and **instance segmentation** (polygons) — accelerated by Meta's
**SAM 2**.

The idea: you annotate the object **once**, by hand, on the first frame of an image
sequence (for example, the frames of a video split into images). SAM 2 then **tracks
that object across the remaining frames**, so for every following frame you just press
a key and correct the result instead of drawing from scratch. Annotations are written
straight to disk in **YOLO format**, ready for training.

![AutoLabel tracking corrosion across the frames of a rotating steel piece: the object is annotated once, then SAM 2 propagates the label across the sequence.](docs/images/corrosion_sample.gif)

> *Annotate the corroded region once on the first frame, then step through the
> sequence letting SAM 2 draw it for you — correcting only where the view has moved
> too far.*

---

## Features

- **Manual annotation**
  - **Rectangle** mode for detection (two clicks) and **Polygon** mode for segmentation
    (click vertices, or *Accept* to finalize).
  - **Vertex editing** — long-press a vertex to drag it, long-press an edge to insert one,
    right-click to delete a vertex or a whole shape.
  - **Per-class colours**, configurable and persisted; shapes highlight on hover.
- **SAM 2 video tracking**
  - Annotate one frame, **Load to Memory**, then **Predict** each following frame on demand.
  - The output **matches what you drew**: a rectangle seed tracks as a **bounding box**, a
    polygon seed tracks as a **polygon** — so detection datasets get boxes, not polygons.
  - When the view drifts too far, **Reset Tracking** and re-seed.
- **YOLO export** — each image gets a `<name>.txt` beside it: detection lines
  (`class cx cy w h`) for boxes, segmentation lines (`class x1 y1 x2 y2 …`) for polygons,
  all normalized. Existing labels load back when you revisit an image.
- **Automatic backend** — the Python SAM 2 service starts and loads the model on launch;
  any failure is reported in an error dialog. A **model picker** lets you switch between the
  SAM 2.1 variants (Tiny / Small / Base+ / Large).
- **On-screen controls** — drawing modes, navigation, and the tracking workflow live on a
  bottom control bar; keyboard shortcuts mirror every button.

---

## How it works

The C++/Qt frontend handles all the UI, drawing, and file I/O. The heavy lifting — SAM 2 —
runs in a **long-lived Python process** that the app launches once and talks to over
**line-delimited JSON on stdin/stdout** (`QProcess`). The model is loaded a single time and
stays resident, and SAM 2's **video predictor** keeps a streaming memory of the object so
each frame is predicted on demand rather than reprocessing the whole sequence.

| Area | Choice |
|---|---|
| Frontend | C++17 · Qt 6 Widgets (hand-coded, no `.ui` files) |
| Backend | Python · PyTorch · SAM 2 (video predictor) |
| Frontend ↔ backend | `QProcess` + line-delimited JSON |
| Mask → geometry | OpenCV (`findContours` / `boundingRect`) in the backend |
| Build | CMake + Ninja |
| Platform | Windows (MinGW) |

---

## Getting started

### Prerequisites

- **Qt 6.5+** (built with 6.11) — Widgets.
- **CMake 3.21+**, **Ninja**, and a **C++17 compiler** (MinGW on Windows).
- **Python 3.10+** with **PyTorch** + **torchvision** (a CPU build is fine) and the packages
  in [`python/requirements.txt`](python/requirements.txt).
- A **SAM 2.1 checkpoint** (downloaded separately — see below).

### Setup

```bash
# 1. Clone with the SAM 2 submodule
git clone --recursive <repo-url>
cd AutoLabel

# 2. Python dependencies (torch/torchvision assumed already installed)
pip install -r python/requirements.txt

# 3. Build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

**Download a model.** Checkpoints are not committed (they're large). Download a SAM 2.1
`.pt` into the `models/` directory — see [`models/README.md`](models/README.md) for links.
**Tiny** is the default and the practical choice on CPU.

```
models/sam2.1_hiera_tiny.pt
```

### Run

```bash
./build/bin/AutoLabel
```

The backend starts and loads the model automatically; the bottom-bar status reads **ready**
once it's up. (Inference runs on the CPU here — expect a couple of seconds per predicted
frame; a GPU is dramatically faster.)

---

## Workflow

1. **File → Open Image Folder…** — pick the folder of frames.
2. Choose **Rectangle** (detection) or **Polygon** (segmentation) on the bottom bar.
3. On the first frame, **draw** the object → **Accept (Enter)** → type its class id.
4. **Load to Memory (M)** — hands the shape to SAM 2.
5. Go to the next frame (→) and **Predict (Space)** — SAM 2 draws it for you. Good enough?
   Move on and press Space again. Drifted? Fix it by hand, or **Reset Tracking** and re-seed.
6. Every frame is saved as a YOLO `.txt` next to the image as you go.

---

## Project structure

```
AutoLabel/
├── CMakeLists.txt           # finds Qt, adds src/
├── src/
│   ├── main.cpp             # entry point + file logger
│   ├── app/                 # MainWindow (UI glue, workflow, export)
│   ├── ui/                  # ImageView (canvas), ControlBar, SettingsDialog
│   ├── annotation/          # Shape, AnnotationModel, ClassColors, YoloIo
│   └── backend/             # SamBackend (QProcess + JSON client)
├── python/
│   ├── sam_service.py       # long-lived SAM 2 service (load / seed / track)
│   └── requirements.txt
├── third_party/sam2/        # SAM 2 (git submodule)
├── models/                  # checkpoints — download here (git-ignored)
└── docs/                    # project documentation + media
```

---

## Documentation

[`docs/index.html`](docs/index.html) is the living project document: what was built and
why, the architectural decisions with code snippets, build instructions, and a per-phase
changelog. Open it in a browser.

---

## Notes & limitations

- Inference runs on the **CPU**, so tracking is on the order of seconds per frame; a CUDA
  build of PyTorch would be far faster (set the `samDevice` setting to `cuda`).
- Tracking handles **one object per memory**; annotate multiple objects by seeding each in
  turn (Reset Tracking between them).
- SAM 2's video predictor expects a coherent **sequence** of frames (e.g. a video split into
  images), not unrelated photos.
