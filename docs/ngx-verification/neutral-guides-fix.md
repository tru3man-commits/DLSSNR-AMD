# Missing Present motion guides must not authorize history

The strongest concrete mismatch found in this inspection is in the optional/never-guides RenoDX Present path. It fabricated a zero-vector texture to satisfy `record_engine`, but the runtime treated the descriptor as a real correspondence source. After the first frame both the pre block and post blend consumed previous-screen-position history, even on camera/object motion. This affects Present without usable guides; it does not explain every game's colour artifacts.

The correction distinguishes descriptor availability from motion availability. Dummy guides close the existing pre/post history gate. Real guides (including genuinely zero vectors), reset, preprocessing-generation checks, history writes, and the noise counter retain their behavior. Exposure adaptation is not reset. This matches the existing documented no-motion pre/post semantics; it is not a newly measured NVIDIA comparison. The identical issue exists in both source trees, so both are corrected deliberately. No shader/ACO records, weights, colour transforms, settings, or memory-guard modifications change.

## Verification

`bash tests/run_temporal_gate.sh`: compiled and executed against each platform's production gate helper; both pass all 16 input states and a six-frame guide-loss/recovery/reset sequence. These are CPU policy tests, not execution of the GPU shaders or the Windows bridge. Static inspection confirms that the flag survives the per-pass EngineFrame copy and that the existing temporal parameter gates both the network's history input and output blend.

The committed 310.9.16 PNG pairs were independently remeasured: 49.1550 / 49.3998 / 50.5916 dB at 1080p / 1440p / 2160p. Those are unchanged upstream baseline assets, NOT results of this patch. The moving-sequence JSON covers synthetic exact-motion sequences, not missing guides. No new PSNR/SSIM gain is claimed.

Local Windows packaging was attempted with `bash windows/build/build_package.sh` and stopped at `no mingw cross compiler (x86_64-w64-mingw32-g++)`. The existing Packages pull-request workflow builds the Windows package and now runs the lightweight test before fetching its dependencies.

## Limits

No Windows game or NVIDIA/AMD GPU run was available locally. With missing engine motion, correct temporal reprojection cannot be recovered from a dummy texture: this fix avoids false history, but cannot reproduce a reference run supplied with real motion. ReShade optical flow also is not equivalent to engine vectors. With valid guides this patch intentionally leaves the existing inference and image output path unchanged. Confirm visually on moving scenes with NRPresentGuides Optional/absent and Never, then verify real-guide and reset cases on GPU.
