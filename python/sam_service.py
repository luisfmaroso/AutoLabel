#!/usr/bin/env python3
"""AutoLabel SAM service.

A long-lived process that talks to the AutoLabel C++ frontend over stdin/stdout
using line-delimited JSON: exactly one JSON object per line in, one per line out.
It is started once and kept alive so SAM2 is loaded a single time and stays
resident -- never reloaded per image.

Protocol
--------
Request : {"command": "<name>", "id": <optional>, ...args}
Response: {"status": "ok"|"error", "message": <str>, "command": <echoed>,
           "id": <echoed if given>, ...}

stdout carries ONLY JSON responses (one per line). All diagnostics go to stderr,
which the C++ side mirrors into its log.

Commands
--------
ping        -> {"status":"ok","message":"pong"}
load_model  -> build the SAM2 predictor once and keep it resident.
              args: checkpoint, config, sam2_root, device
segment     -> run SAM2 on an image with point prompts, return polygon(s).
              args: image_path, points [[x,y],...], labels [1|0,...]
shutdown    -> exit the loop.

torch / sam2 are imported lazily inside load_model, so `ping` works without the
heavy ML stack present.
"""

import json
import sys

# Resident state -- the whole point of a long-lived process.
_predictor = None          # SAM2ImagePredictor (image mode)
_image_path = None         # path currently set on the predictor (embedding cache)

# Video propagation state.
_video_predictor = None    # SAM2 video predictor (built lazily on first propagate)
_frames_dir = None         # temp dir of numeric JPEGs staged for the video API
_frames_sig = None         # signature of the staged frame list (rebuild only if it changes)
_track_state = None        # resident inference_state (holds conditioning/memory frames)
_track_state_sig = None    # which staged frames the resident state belongs to
_track_gen = None          # live propagate_in_video generator, stepped one frame per request
_track_cache = {}          # frame_idx -> polygon, for frames already yielded
_track_wh = (0, 0)         # (width, height) of the staged frames

# Remembered model args from load_model, reused to build the video predictor.
_model_cfg = None
_model_ckpt = None
_model_device = "cpu"
_model_sam2_root = None


def log(message: str) -> None:
    print(f"[sam_service] {message}", file=sys.stderr, flush=True)


def _load_model(req: dict) -> dict:
    global _predictor, _image_path
    global _model_cfg, _model_ckpt, _model_device, _model_sam2_root, _video_predictor
    import os

    checkpoint = req["checkpoint"]
    config = req.get("config", "configs/sam2.1/sam2.1_hiera_t.yaml")
    sam2_root = req.get("sam2_root")
    device = req.get("device", "cpu")

    if sam2_root and sam2_root not in sys.path:
        sys.path.insert(0, sam2_root)  # make the vendored submodule importable

    if not os.path.isfile(checkpoint):
        return {"status": "error", "message": f"checkpoint not found: {checkpoint}"}

    log(f"loading SAM2: config={config} checkpoint={checkpoint} device={device}")
    import torch  # noqa: F401 (ensures a clear error if torch is missing)
    from sam2.build_sam import build_sam2
    from sam2.sam2_image_predictor import SAM2ImagePredictor

    model = build_sam2(config, checkpoint, device=device)
    _predictor = SAM2ImagePredictor(model)
    _image_path = None

    # Remember args so propagate can build the video predictor with the same model.
    _model_cfg, _model_ckpt, _model_device, _model_sam2_root = config, checkpoint, device, sam2_root
    _video_predictor = None  # force a rebuild against the new model

    log("SAM2 ready")
    return {"status": "ok", "message": f"model loaded (device={device})", "device": device}


def _polygon_to_mask(polygon, height, width):
    import cv2
    import numpy as np
    mask = np.zeros((height, width), dtype=np.uint8)
    pts = np.asarray(polygon, dtype=np.int32).reshape(-1, 1, 2)
    cv2.fillPoly(mask, [pts], 1)
    return mask


def _mask_to_polygon(mask_uint8):
    """Largest external contour of a 0/1 mask, simplified -> [[x,y],...] or None."""
    import cv2
    contours, _ = cv2.findContours(mask_uint8, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return None
    c = max(contours, key=cv2.contourArea)
    if cv2.contourArea(c) < 25:
        return None
    peri = cv2.arcLength(c, True)
    approx = cv2.approxPolyDP(c, max(2.0, 0.01 * peri), True)
    poly = [[int(p[0][0]), int(p[0][1])] for p in approx]
    return poly if len(poly) >= 3 else None


def _stage_frames(frames):
    """SAM2's video API needs a folder of JPEGs named <index>.jpg. Build one
    (cached, rebuilt only when the frame list changes) and return its path."""
    global _frames_dir, _frames_sig
    import os
    import shutil
    import tempfile
    import cv2

    sig = "\n".join(frames)
    if sig == _frames_sig and _frames_dir and os.path.isdir(_frames_dir):
        return _frames_dir

    if _frames_dir and os.path.isdir(_frames_dir):
        shutil.rmtree(_frames_dir, ignore_errors=True)

    _frames_dir = tempfile.mkdtemp(prefix="autolabel_frames_")
    for i, path in enumerate(frames):
        img = cv2.imread(path)
        if img is None:
            raise RuntimeError(f"failed to read frame: {path}")
        cv2.imwrite(os.path.join(_frames_dir, f"{i:05d}.jpg"), img,
                    [int(cv2.IMWRITE_JPEG_QUALITY), 95])
    _frames_sig = sig
    log(f"staged {len(frames)} frames -> {_frames_dir}")
    return _frames_dir


def _reset_tracking(req: dict) -> dict:
    """Forget all memory so the user can start a new object/segment from scratch."""
    global _track_state, _track_state_sig, _track_gen, _track_cache
    _track_state = None
    _track_state_sig = None
    _track_gen = None
    _track_cache = {}
    return {"status": "ok", "message": "tracking reset"}


def _track_seed(req: dict) -> dict:
    """Step 3: load the current frame's polygon into SAM2 memory (a conditioning
    frame). Subsequent track_step calls predict other frames from this memory."""
    global _video_predictor, _track_state, _track_state_sig
    global _track_gen, _track_cache, _track_wh
    import cv2
    import torch

    if _model_ckpt is None:
        return {"status": "error", "message": "model not loaded; send load_model first"}

    frames = req.get("frames", [])
    seed_frame = int(req.get("seed_frame", 0))
    polygon = req.get("polygon", [])
    if len(frames) < 1:
        return {"status": "error", "message": "no frames"}
    if len(polygon) < 3:
        return {"status": "error", "message": "seed polygon needs >= 3 points"}
    if not (0 <= seed_frame < len(frames)):
        return {"status": "error", "message": "seed_frame out of range"}

    seed_img = cv2.imread(frames[seed_frame])
    if seed_img is None:
        return {"status": "error", "message": f"failed to read seed frame: {frames[seed_frame]}"}
    h, w = seed_img.shape[:2]
    _track_wh = (w, h)

    if _video_predictor is None:
        from sam2.build_sam import build_sam2_video_predictor
        log("building SAM2 video predictor")
        _video_predictor = build_sam2_video_predictor(_model_cfg, _model_ckpt, device=_model_device)

    frames_dir = _stage_frames(frames)

    with torch.inference_mode():
        # Re-init the state only when the frame set changes; otherwise reuse it so
        # several seeds (added on different frames) accumulate as memory.
        if _track_state is None or _track_state_sig != _frames_sig:
            _track_state = _video_predictor.init_state(video_path=frames_dir)
            _track_state_sig = _frames_sig
            log("tracking state initialised")
        _video_predictor.add_new_mask(
            _track_state, frame_idx=seed_frame, obj_id=1,
            mask=torch.as_tensor(_polygon_to_mask(polygon, h, w), dtype=torch.bool))

    # Memory changed -> any in-progress propagation/cached frames are stale.
    _track_gen = None
    _track_cache = {}
    log(f"seeded memory at frame {seed_frame}")
    return {"status": "ok", "message": f"seeded memory at frame {seed_frame}",
            "width": int(w), "height": int(h)}


def _track_step(req: dict) -> dict:
    """Step 4: predict ONE frame from the current memory. Drives a forward
    propagation generator one frame at a time (cheap, on demand)."""
    global _track_gen, _track_cache
    import numpy as np
    import torch

    if _track_state is None:
        return {"status": "error", "message": "no memory; load a polygon first (track_seed)"}

    target = int(req.get("target", -1))
    w, h = _track_wh

    if target in _track_cache:
        poly = _track_cache[target]
        return {"status": "ok", "message": "cached", "frame": target,
                "width": int(w), "height": int(h), "polygon": poly or []}

    with torch.inference_mode():
        if _track_gen is None:
            _track_gen = _video_predictor.propagate_in_video(_track_state)
        found = None
        while True:
            try:
                fidx, _obj_ids, mask_logits = next(_track_gen)
            except StopIteration:
                break
            m = (mask_logits[0, 0] > 0.0).cpu().numpy().astype(np.uint8)
            _track_cache[int(fidx)] = _mask_to_polygon(m)
            if int(fidx) == target:
                found = _track_cache[int(fidx)]
                break

    if found is None and target not in _track_cache:
        return {"status": "ok", "message": "frame not reachable from memory (re-seed?)",
                "frame": target, "width": int(w), "height": int(h), "polygon": []}
    poly = _track_cache.get(target)
    return {"status": "ok", "message": ("predicted" if poly else "object lost on this frame"),
            "frame": target, "width": int(w), "height": int(h), "polygon": poly or []}


def _segment(req: dict) -> dict:
    global _image_path
    import os
    import cv2
    import numpy as np

    if _predictor is None:
        return {"status": "error", "message": "model not loaded; send load_model first"}

    image_path = req["image_path"]
    points = req.get("points", [])
    labels = req.get("labels", [])
    if not points:
        return {"status": "error", "message": "no prompt points given"}
    if len(points) != len(labels):
        return {"status": "error", "message": "points and labels length mismatch"}
    if not os.path.isfile(image_path):
        return {"status": "error", "message": f"image not found: {image_path}"}

    # Set (and cache) the image so repeated clicks on the same image don't
    # recompute the expensive image embedding.
    if image_path != _image_path:
        bgr = cv2.imread(image_path)
        if bgr is None:
            return {"status": "error", "message": f"failed to read image: {image_path}"}
        rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
        _predictor.set_image(rgb)
        _image_path = image_path
        log(f"image set: {image_path} {rgb.shape}")

    masks, scores, _ = _predictor.predict(
        point_coords=np.asarray(points, dtype=np.float32),
        point_labels=np.asarray(labels, dtype=np.int32),
        multimask_output=True,
    )
    best = int(np.argmax(scores))
    mask = (masks[best] > 0).astype(np.uint8)
    h, w = mask.shape[:2]

    # Mask -> polygons via contour extraction + simplification (Phase 8 logic,
    # done here in the backend where cv2 already lives).
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    polygons = []
    for c in sorted(contours, key=cv2.contourArea, reverse=True):
        if cv2.contourArea(c) < 25:  # drop specks
            continue
        peri = cv2.arcLength(c, True)
        approx = cv2.approxPolyDP(c, max(2.0, 0.01 * peri), True)
        poly = [[int(p[0][0]), int(p[0][1])] for p in approx]
        if len(poly) >= 3:
            polygons.append(poly)

    return {
        "status": "ok",
        "message": f"{len(polygons)} polygon(s)",
        "score": round(float(scores[best]), 4),
        "width": int(w),
        "height": int(h),
        "polygons": polygons,
    }


def handle(request: dict) -> dict:
    command = request.get("command")
    if command == "ping":
        return {"status": "ok", "message": "pong"}
    if command == "load_model":
        return _load_model(request)
    if command == "segment":
        return _segment(request)
    if command == "track_seed":
        return _track_seed(request)
    if command == "track_step":
        return _track_step(request)
    if command == "reset_tracking":
        return _reset_tracking(request)
    if command == "shutdown":
        return {"status": "ok", "message": "bye", "_shutdown": True}
    return {"status": "error", "message": f"unknown command: {command!r}"}


def main() -> int:
    log(f"started (python {sys.version.split()[0]})")
    for line in sys.stdin:
        line = line.lstrip("﻿").strip()  # tolerate a stray leading BOM
        if not line:
            continue

        try:
            request = json.loads(line)
        except json.JSONDecodeError as exc:
            sys.stdout.write(json.dumps({"status": "error",
                                         "message": f"invalid JSON: {exc}"}) + "\n")
            sys.stdout.flush()
            continue

        request_id = request.get("id")
        try:
            response = handle(request)
        except Exception as exc:  # never let one bad request kill the service
            response = {"status": "error", "message": f"{type(exc).__name__}: {exc}"}

        # Echo command + id so the C++ side can route the reply.
        if "command" in request and "command" not in response:
            response["command"] = request["command"]
        if request_id is not None:
            response["id"] = request_id

        shutdown = response.pop("_shutdown", False)
        sys.stdout.write(json.dumps(response) + "\n")
        sys.stdout.flush()

        if shutdown:
            log("shutting down")
            break

    return 0


if __name__ == "__main__":
    sys.exit(main())
