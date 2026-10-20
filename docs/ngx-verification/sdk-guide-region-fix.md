# SDK-guided motion-region normalization fix

Base: user's `tru3man-commits/DLSSNR-AMD`, main `765bfb7` (includes the neutral-motion temporal gate fix and experimental memory-guard changes). This change does not replace those commits with upstream main.

## Evidence from the supplied SDK

Inspected the user's `streamline-sdk-v2.14.16 (1).rar` locally. Relevant supplied files:

- `docs/ProgrammingGuide.md`, Resource Extent: resource suballocations have an origin and active width/height, separate from the allocation.
- `include/sl_consts.h`, `Constants::mvecScale`: converts stored vectors to normalized displacement; jitter and reset are separate properties.
- `source/plugins/sl.dlss/dlssEntry.cpp`, NGX evaluation: motion scaling uses render dimensions; it separately forwards jitter, pre-exposure and exposure scale.
- `external/ngx-sdk/include/nvsdk_ngx_defs_vk.h`: image descriptor width/height describe the resource, not an arbitrary active subrect.

These establish the distinction between allocation, active region and normalized motion. DLSS-SR parameter names/defaults are NOT assumed to be DLSS-NR's contract. This archive includes a root `nvngx_dlssnr.dll`, but no `sl_dlss_nr.h` or `sl.dlss_nr` plugin source was found in its inventory. Its DLL was not executed, modified, or committed, and no model data was extracted or redistributed.

The actual defect is independently established in this repository: `apply_guide_subrect` samples the remaining allocation when an extent is zero, clips oversized extents, and falls back to the full allocation for invalid origins. `nr_dlssnr_model.cpp` normalized vectors BEFORE those rules, using raw requested extents or full allocation sizes. Thus sampling and displacement used different coordinate systems.

## Change

A small platform-local `nr_guide_region.hpp` implements the existing region rules once. Both session sampling-region setup and model motion normalization use it. D3D12 and Vulkan evaluation are corrected in both Windows and Linux trees.

Example: allocation 3840x2160, origin (1920,1080), implicit remaining extent. Sampling covers 1920x1080. Previously X scale 1 became 1/3840; now it becomes 1/1920. An 8-pixel backward vector formerly sampled history only 4 active-region pixels away, instead of 8.

Explicit valid extents and ordinary full-frame allocations keep the same division and values. This also fixes oversized/clipped and out-of-bounds-origin fallback mismatches without changing how the region itself is selected. Zero-sized resources preserve the old divide-by-zero avoidance. No colour/exposure, jitter, weights, shaders, seed, history blend, network precision or ACO record changes.

The earlier fallback-search patch from a separate upstream checkout is not silently imported here; this branch is based on the user's fork and addresses an SDK-guided engine-vector integration defect.

## Validation performed

Commands:

```sh
bash tests/run_guide_region.sh
bash tests/run_temporal_gate.sh
```

Both platform copies pass:

- 50 region/scale cases: full allocations, valid explicit offset regions, implicit remainder, clipped extents, invalid X/Y origins, one-texel regions, maximum integer extents, empty dimensions; zero, positive, negative and fractional scales.
- 32-frame synthetic coordinate replay: first frame stationary, then exact +8-pixel backward motion in an offset implicit region. Mean displacement error: **3.875 active-region pixels before, 0 after** (31 moving frames: **4 -> 0**). This is a geometric CPU component metric, NOT final NR output PSNR or a GPU benchmark.
- Existing 16-state temporal-gate truth table and six-frame guide loss/recovery/reset sequence continue to pass. This confirms unchanged gate semantics; it does not establish full temporal image quality.

The new test is added to the existing package workflow alongside the prior gate test.

## Remaining limits

No live games tested. No local GPU, Windows runtime, new NVIDIA reference rendering or model inference run. No claim of measured final image improvement, game-specific benefit, or complete temporal stability. The bug affects calls using implicit/clipped/fallback motion subrects; ordinary full-frame or valid explicit-region calls should be unchanged. The exact frequency in the user's games is unknown.

A game-level comparison still requires matched colour/HDR-exposure, guide region metadata, motion, reset and seed sequences on old/new builds. Compare reprojected-history alignment and moving/disocclusion regions, not just static PSNR. Keep this a draft until CI builds and target-game A/B are reviewed. The SDK DLL alone cannot provide a reference render without a compatible NVIDIA execution environment.
