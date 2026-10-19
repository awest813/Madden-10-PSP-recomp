# Native GE replay performance

This diagnostic replays extracted NCAA graphics records through the production GE parser and GLES renderer. It does not execute guest CPU instructions, load an ISO, or measure browser/gameplay FPS. Game dumps, accepted captures and executables remain local under ignored `build/`.

## Build and run

From an existing configured native NCAA build with `PSPWEB_HOST_TESTS=ON`:

```powershell
cmake --build build/native-ncaa10 --target pspweb_ge_replay -j 3
build/native-ncaa10/profiles/web/pspweb_ge_replay.exe commands.bin pushbuffer.bin output.ppm --frames 64 --warmup 32 --raw-display display565.bin --no-eco
```

`--no-eco` is Windows-only and opts just this diagnostic process out of execution-speed throttling. It does not change global power settings or production behavior. Omit it on other platforms. Inputs/options are validated before replay; numeric options are parsed once, outside the record loop. Guest RAM is explicitly 64 MiB. RGBA channels are verified at eight bits or higher. Timed frames include parsing, rendering, EGL pbuffer presentation/swap and GPU completion; image downloads and output files follow timing. A pbuffer swap permits driver frame cleanup without opening a window.

For a matched comparison, build baseline and candidate with the **same version of this harness, compiler, optimization flags and core runtime**. Use accepted port framebuffer output as the expected image; a PPSSPP reference can differ because remaining fidelity work is separate from a performance-only change. Write a local scene manifest:

```json
{
  "disc_id": "ULUS10440",
  "guest_ram_mib": 64,
  "scenes": [{
    "name": "gt-full",
    "commands": "build/capture/commands.bin",
    "pushbuffer": "build/capture/pushbuffer.bin",
    "expected_display": "build/accepted/gt-display565.bin"
  }]
}
```

Paths are absolute or relative to the source root. The expected framebuffer is the full 272-row RGB565 image, including its 480–1024-pixel stride. Then run:

```powershell
python scripts/benchmark_ge_replay.py --baseline build/baseline-replay.exe --candidate build/native-ncaa10/profiles/web/pspweb_ge_replay.exe --scenes build/scenes.json --output build/replay-comparison --frames 64 --warmup 32 --no-eco
```

The output directory must be new and below `build/`. The runner performs ABBA order per scene, records executable/input/framebuffer hashes, retains all samples/logs/images, excludes explicit warmup, and reports median/p95/maximum plus draw/copy/download/metadata counters. It rejects mismatched framebuffers, presentation that differs from direct RGB565, wrong title/RAM metadata, invalid samples and unconfirmed requested power control. Baseline and candidate each need their own executable; the runner never rebuilds or overwrites them. Do not run other builds/replays concurrently. Run `python scripts/test_ge_replay_benchmark.py` for the asset-free runner regressions and `python scripts/test_ge_replay_cli.py --exe build/native-ncaa10/profiles/web/pspweb_ge_replay.exe` for a synthetic red-frame/invalid-input smoke test.

## Measurement correction and current evidence

The older local harness queried debug environment variables inside every record submission. Caching those options before replay reduced roughly 150–180 ms runs to roughly 50–53 ms with the same renderer and identical pixels. That is diagnostic overhead removal, **not a gameplay speedup**. Previous absolute replay times and percentage regressions should not be treated as isolated renderer cost. The new harness exposes warmup and completes EGL frames; retain older evidence with its method instead of relabeling it.

The accepted renderer change uses immutable atlas storage, uploads only used cells for a one-row metadata batch, and caches texture/sampler binding state. Multi-row batches remain one upload; the 64-cell addressing grid, three-atlas rotation and all sampling/blend/feedback semantics are unchanged. Rendering adds no CPU readbacks. Tests mix partial and full rows across atlas reuse. Fourteen complete/prefix captures remain bit-identical to `fbc80e7`, including RGBA8 presentation matching direct RGB565.

Final matched O3/DNDEBUG ABBA, **64 frames/run with 32 warmup frames excluded**, the same new harness/compiler/core on both sides, no concurrent replay/build, and process-local no-Eco control: GT median **47.51845 → 46.27640 ms (-2.61%)**; SDSU **51.30980 → 51.43310 ms (+0.24%)**. GT p95/max **51.1588/57.9431 → 51.1503/55.4914 ms**; SDSU **54.5392/56.6044 → 56.7274/66.0198 ms**. SDSU's median is essentially flat and its tail worsens; do not claim a universal speedup. Metadata bytes/frame fall GT **3,187,712 → 814,928 (74.44% less)** and SDSU **3,820,544 → 933,648 (75.56% less)**. Draws/copies/copied pixels remain **1,917 / 825 / 1,069,788** and **1,973 / 910 / 863,209**. Final direct-image download is outside timing; neither scene adds rendering downloads. These are native replay measurements, not browser/gameplay FPS.

Evidence on the original machine: `build/replay-perf-20261014/`, including baseline source snapshot, profiler logs, rejected prototypes, matched runs and capture hashes. Flat integer vertex attributes, small uniform blocks and per-draw atlas rotation were measured and rejected as slower. Initial screening/matched runs were noisy; use the final recorded method and report regressions or variance alongside improvements. Browser pacing/FPS/audio and wider device coverage remain unverified by this tool. Default NCAA RAM stays 64 MiB / 56 MiB user partition.
