# Windows port of the 0.0.8 processing path

This branch brings the Linux `v0.0.8-linux-preview` reference (commit
`960038fd1cb892742548f408355e0f3b2fd30d6f`) to the separate native Windows tree.
It is a preview requiring Windows GPU testing, not a claim of visual parity or a
flicker-free release. This is the Mochizuki-derived ReShade NR project; the
separate Worker B launcher is not part of this repository.

## Included

- Wilsjo2 processing, preprocessing, fast passes, layer controls, comparison views,
  HUD composition and HDR10 conversion from the reference implementation.
- Native D3D10/11/12 shared-image handover for these passes. D3D12 uses the game's
  queue and explicit resource transitions; D3D11 uses shared fences; D3D10 uses
  keyed mutexes. DX9 retains the existing DXVK route.
- Windows LLPC shader adaptations and existing ACO records. The build rejects a
  missing record rather than silently shipping an incomplete ACO set.
- Earlier temporal guide-region and bridge submission fixes. Known pending GPU
  completion proofs cannot expire into premature descriptor or image reuse.
- The installer, model extraction tools and three existing Windows networks.
  NVIDIA models and the source NVIDIA DLL are not distributed.

## Limits

`CC_SMALL_FP8_EXPERIMENTAL` is unavailable on Windows because its matching Windows
ACO records are absent. The existing OptiScaler-NR route remains; the Linux 0.0.8
OptiScaler ASI integration is not a native Windows port. Windows NR Control remains
platform-specific; use ReShade's Add-ons page for the new layer controls.

CPU tests and cross-compilation cannot validate GPU synchronization, AMD driver
compatibility, game-specific resource formats or HDR appearance. The upstream
README's GPU measurements and tested driver versions describe upstream testing,
not measurements of this port.

## Installation for ReShade

1. Extract the complete Windows ZIP into a writable folder.
2. Run `install.bat`, select the actual 64-bit game executable, and choose
   `reshade-feeder` for DX10/11/12, `reshade-feeder-dx9` for DX9, or
   `reshade-feeder-vulkan` for Vulkan.
3. Supply your own supported `nvngx_dlssnr.dll` when prompted. Keep the default
   `CC_SMALL_FP8` and optional styles off for the first comparison.
4. Open ReShade with Home and use its Add-ons page. Verify the original image
   with NR disabled, then enable NR. Retain existing settings and logs before
   changing processing options.
5. `install.bat` option 6 collects diagnostics; option 5 uninstalls and restores
   the installer's backups.

## Validation

Run `bash tests/run_temporal_gate.sh`, `bash tests/run_guide_region.sh`,
`bash tests/run_bridge_submit.sh` and `bash tests/run_gpu_completion.sh`.
Build with `bash fetch_deps.sh --windows` and
`bash windows/build/build_package.sh`. The package build checks every ACO network
shader against its record, including the optional int4 set.

Before marking a release ready, test DX11, DX12 and Vulkan on Windows with an
RX 9000 GPU. Exercise model switches, layer toggles, resize, alt-tab, loading
screens, guide loss, HUD composition and repeated enable/disable cycles. Test
SDR and HDR10 separately, including 2560x1440 HDR10 on an RX 9070 XT, keeping the
same scene and settings for comparison. Record logs and frame times; check for
device removal, stale frames, flicker and abnormal VRAM growth. D3D10 keyed-mutex
and DX9/DXVK paths need their own runtime tests.
