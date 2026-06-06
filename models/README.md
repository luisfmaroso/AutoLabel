# SAM2 model checkpoints

This folder holds SAM2 checkpoint weights (`*.pt`). They are **not** committed to
git (too large) — download them here yourself.

The SAM2 *code* lives in the `third_party/sam2` submodule; these `.pt` files are
just the weights it loads.

## Recommended (CPU): tiny

Download into this folder:

- **`sam2.1_hiera_tiny.pt`** &mdash; https://dl.fbaipublicfiles.com/segment_anything_2/092824/sam2.1_hiera_tiny.pt

It pairs with the config `configs/sam2.1/sam2.1_hiera_t.yaml` (shipped inside the
submodule). On a CPU-only machine, tiny is the practical choice.

## Other sizes (slower on CPU)

| Checkpoint | Config |
|---|---|
| `sam2.1_hiera_small.pt`      | `configs/sam2.1/sam2.1_hiera_s.yaml`  |
| `sam2.1_hiera_base_plus.pt`  | `configs/sam2.1/sam2.1_hiera_b+.yaml` |
| `sam2.1_hiera_large.pt`      | `configs/sam2.1/sam2.1_hiera_l.yaml`  |

(All under `https://dl.fbaipublicfiles.com/segment_anything_2/092824/<name>.pt`.)

The checkpoint path is configurable from the app (Phase 5), defaulting to
`models/sam2.1_hiera_tiny.pt`.
