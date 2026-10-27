# Windows bridge submission lifecycle

Baseline: `c476df6d6939058dd3e62c636c1a224169a66161`.

## Reproduced defects

The native Windows DirectX routes copy the game's inputs into shared textures,
record the Vulkan network into a bridge job, submit that job between the DirectX
fence signals, then copy the result back. D3D10 uses keyed mutexes instead of
timeline semaphores. Both submit paths must keep the recorded job alive and
must reject a discarded job before it can reach Vulkan.

1. **Rejection depended on an optional callback.** `drop_locked()` marked a job
   free, but `submit()` and `submit_keyed()` returned early only when the callback
   vector was nonempty. Without an `on_drop` callback, an already rejected job
   still reached `vkQueueSubmit`. This also affected submissions after device
   loss. Rejection now returns false unconditionally, after notifying any owner.
2. **The job was unlocked during submission.** Between generation validation
   and publication of `Submitted`, `discard()`/`cancel()` could mark the job
   free, allowing `begin()` to reset/reuse its command buffer. Both submit paths
   now keep `jobs_mutex_` held through `vkQueueSubmit` and the state update.
   The queue remains externally synchronized; callbacks run after both locks
   have been released. No GPU fence wait was added to the normal submit path.
3. **D3D10 did not latch Vulkan device loss or reject stale recordings.** The
   keyed-mutex route now observes the same 48-recording cutoff and device-loss
   stop condition as the timeline route. It does not attempt automatic recovery
   of a lost Vulkan device.
4. **Recording/submission counters were process-global.** Different devices
   accessed them under different mutexes. Besides the data race, activity on one
   adapter could incorrectly age another adapter's recorded frame. The counters
   are now per-device under `jobs_mutex_`. One-time diagnostics in these submit
   methods use atomic flags because they remain shared across devices.

## Regression tests

Run `bash tests/run_bridge_submit.sh`. The test extracts the actual production
`Job` definition, Device job members and submit/drop/discard/cancel methods into
a temporary translation unit and compiles them against a fake Vulkan queue.
It does not maintain a separate implementation of the lifecycle. The extractor
fails if a required definition disappears.

Coverage includes stale/lost jobs with and without callbacks, the exact age
boundary, two adapters, invalid generations, duplicate submissions, discarded
jobs, injected queue failures, persistent device loss, lock ownership during
submission, callback reentry, out-of-order history notification and preservation
of timeline values/keyed-mutex payloads. The lock probe runs on another thread.
The runner has a timeout so a callback deadlock fails CI instead of hanging it.

The same harness against the baseline source reports **22 failed checks**;
the patched source reports **85 checks, zero failures**. Total check counts
differ because rejected frames no longer enter the fake Vulkan function or
invoke callbacks on paths they should not take. To replay the baseline, export
its `windows/src/pe/nr_pe_bridge.{cpp,hpp}` into a directory and pass that directory
as the runner's first argument.

The existing temporal gate and guide-region tests also pass for both platform
copies. All three runners now execute in a separate CPU test job before either
package build, including when Windows packaging is disabled.

## Limits

These tests establish CPU-side submission/lifetime behavior, not Vulkan driver
correctness, GPU completion timing or picture quality. No game was run, no new
NVIDIA-reference PSNR/SSIM was measured, and no FPS improvement is claimed.
This patch does not establish that these bugs caused the reported Worker B
shutdowns or flicker in an external AMDNR host. Those symptoms still require
matched host/runtime logs and a Windows GPU A/B run.

Network weights, shaders, color/exposure transforms, motion-vector scaling and
the existing memory-check policy are unchanged. Changes are confined to the
native Windows bridge; the Linux implementation has no equivalent bridge.
