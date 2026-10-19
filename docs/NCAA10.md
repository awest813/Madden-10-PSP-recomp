# NCAA Football 10 PSP web port (ULUS10440)

**Verified: actual kickoff and scrimmage gameplay in the browser.** The supplied
US PSP disc is statically recompiled to C++ and WebAssembly using this project's
God of War: Chains of Olympus web runtime. It is NCAA Football 10's own executable,
assets, menus, players, and game logic. No PSP interpreter or JIT is used.

**64 MiB is the default:** open `http://127.0.0.1:8621/` normally, or add
`?profile` for timing. The game gets 64 MiB guest RAM and a 56 MiB user
partition. NCAA grows MAIN from 16,991,184 to 32,759,760 physical bytes
through its own heap initialization.
Native builds also default to 64 MiB when `PSPWEB_RAM_MB` is unset. Use
`?ram=32` in the browser or `$env:PSPWEB_RAM_MB='32'` natively for the
original 32 MiB layout; an explicit `64` is also accepted. The larger layout
passed the cold Fresno State–San Diego State Qualcomm DAY load and a fresh UK–FSU browser gameplay
test. This mitigates the original 32 MiB fragmentation failure; its root
cause remains open. See [the heap comparison and evidence](NCAA10-CRASH-PROVENANCE.md#larger-heaps-2026-10-12).

## Verified game identity

`scripts/verify_ncaa10.py games/ncaa10/root` checks both the disc metadata and
exact decrypted executable before the build proceeds:

- PARAM.SFO: `NCAA® FOOTBALL 10`, `ULUS10440`, PSP system version `5.50`.
- Decrypted EBOOT: 8,415,696 bytes; SHA-256
  `2b743dc6d746d822969a9b967b13422a369ff03c6a3a3e2604e051c1642b94f6`.
- Internal ELF module name: `NCAA2007`. This is the name inside this verified
  NCAA Football 10 executable, not evidence that another game's code was used.
- Entry `0x08B6EE80`, 258 imports, 234,645 relocations, 335,450 registered entries.

Run commands from the repository root containing `scripts` and `profile`.
In the original development workspace this is the inner `psp-web-recomp-main`
directory. Fresh clones should start with the [README build instructions](../README.md#build-ncaa-from-a-fresh-checkout). Relevant folders are:

| Folder | Role |
|---|---|
| `games/ncaa10/root` | Verified decrypted executable, extracted NCAA disc and raw UMD image |
| `profile/generated/ncaa10` | NCAA executable translated ahead of time into C++ |
| `profile/generated/ncaa10_sound` | Ahead-of-time translations of EA audio-bank programs |
| `PSPRecomp` | Static recompiler source, including the local compiler fixes |
| `profile/host` | PSP kernel, file I/O, graphics, audio and utility implementation |
| `profile/web/shell.html` | Browser canvas, controls, audio, save and keyboard UI |
| `build/web-ncaa10/profiles/web` | Built HTML, JavaScript, WebAssembly and preload data |
| `scripts` / `patches` | Extraction/build/verification tools and compiler patch records |

## Launch the existing build

From the project directory (the inner `psp-web-recomp-main` folder):

```powershell
python scripts/serve.py build/web-ncaa10/profiles/web 8621 --mount /disc=games/ncaa10/root/disc
```

Open <http://127.0.0.1:8621/>. The server supplies the cross-origin isolation
headers required by the WebAssembly graphics worker and HTTP Range requests
for the disc. Opening `index.html` directly from the filesystem will not work.
Port 8621 is the recorded local development origin; check whether a server is running before reusing it.

The executable and manifest are preloaded (about 8.44 MB); the other disc assets
stream locally as requested. `disc/__umd.iso` backs the game's raw UMD reads.
Keep the extracted disc and this raw image in place.

## Controls and path into a match

| Keyboard | PSP input |
|---|---|
| Enter | Start |
| Z / X / A / S | Cross / Circle / Square / Triangle |
| Arrow keys | D-pad |
| I / J / K / L | Analog up / left / down / right |
| Q / W | L / R |
| Shift | Select |

Gamepads are supported by the existing web shell. To reach a match: Enter at
the title screen, Enter on the Road to Glory information screen, Z to accept
the favorite-team choice, then Enter to continue. Choose **Play Now**, confirm
the teams, assign the controller, and confirm. Allow the stadium to load; Enter
skips the stadium introduction. Follow the coin-toss choices with Z.

At the kickoff play screen, Z selects **Kickoff Middle**. Pull the analog stick
back with K, then push forward with I to kick. A roughly half-second press in
each direction was sufficient in the browser test. During regular play, the
analog keys move the selected player.

## Build again on this Windows machine

Requires Python 3, CMake, Ninja, Clang, and an installed Emscripten SDK. The
current machine uses Clang 22 and Emscripten 6.0.10. The existing decrypted
`games/ncaa10/root/EBOOT.BIN` must be retained. No game data is distributed.

```powershell
scripts/build_ncaa_web.ps1 -Iso "NACC_Football_10_USA_PSP-PSPKiNG.iso"
# Build and start the local server:
scripts/build_ncaa_web.ps1 -Iso "NACC_Football_10_USA_PSP-PSPKiNG.iso" -Serve
```

Use `-Emsdk C:/path/to/emsdk` to select another SDK. Add `-Generate` to regenerate
the full executable corpus and the 22 distinct Allegrex audio programs found
in 88 EA bank entries. This can take substantially longer than an incremental
build. All seven checked-in framework patches are applied by `scripts/setup.sh`,
including the NCAA compiler fixes in patch 6 and runtime provenance in patch 7.
The original local framework already contains them. Existing generated sources and build products are local
and ignored by version control.

The script validates the title, extracts the disc if necessary, verifies the
raw streaming image by SHA-256 (unpacking ZIP input when needed), writes the manifest, packages only the executable/manifest, and links
`build/web-ncaa10/profiles/web/index.html`, `.js`, `.wasm`, and `.data`. Link
optimization is set to zero to avoid a very long whole-corpus wasm-opt pass;
the generated C++ units are compiled at optimization level one.

## Changes that enabled gameplay

- Correct UMD sector units and 64-bit seek argument alignment; streamed raw-disc
  reads and `/sce_lbn..._size...` byte windows.
- Correct thread CPU time, Vblank sub-interrupt callbacks, alarms, and distinct
  thread termination/deletion behavior used by EA's I/O and sound systems.
- The exported `sce_module_start_thread_parameter` requests a **1 KB** stack.
  Honoring it instead of reserving the default 256 KB leaves enough guest heap
  for the gameplay runs documented below. This resolved the original blanket
  loading failure; the intermittent daytime San Diego State null-buffer failure
  found during later regression remains open.
- `vi2s` VFPU packing and ahead-of-time compilation of executable audio-bank
  programs, including guest-relocated jumps and delay slots.
- Windows manifest newline handling and controller startup after WebAssembly
  initialization. Windows native validation uses ANGLE EGL/GLES.
- A guarded NCAA-specific skip for the optional intro movie routine, because
  the host does not yet decode AVC video.
- A guarded replacement for the title's display polling loop. It sleeps until
  the next Vblank only at the verified wait caller (`0x08C9BBC4`) while the
  registered EA handler (`0x08C9BB78`) is enabled and its counter is below the
  frame object's target. Other calls execute the original generated function.
  `PSPWEB_NO_IDLE_ACCEL=1` disables this native diagnostic optimization.
- Display buffer latching distinguishes immediate and next-Vblank requests,
  including same-address redraws. Guest time uses the exact rational PSP
  Vblank period; the browser uses bounded catch-up and resets timing after a
  hidden tab. NCAA presentation follows committed game frames.

The [LCS recomp](https://github.com/elmasas/lcs-recomp) was reviewed as a
reference. This port uses the existing PSP Web Recomp architecture rather than
GTA's game-specific code.

## Correlated crash provenance

The latest cold Qualcomm DAY reproduction connects the failed `39480`
animation allocation, owner/stream fields, queued descriptor, reader stack
slot and final `08C7EE50` store with one provenance ID. The allocation returns
zero because the largest free block is too small, despite sufficient total
free space. See [the complete provenance report](NCAA10-CRASH-PROVENANCE.md)
for the five checkpoints, exact writers, decoded delay-slot trace, raw
artifacts and validation. The fragmentation cause remains open.

The current `UserSbrk` reservation is `08EC9000 + 01036400`, after correcting
LowAligned/HighAligned partition allocation. Older diagnostic reservations
below describe earlier builds. The correction matches the initial official
PPSSPP reference layout but does not eliminate this cold DAY failure.

## Validation and current limits

Browser verification completed the title/menu/team/controller path, stadium
loading, coin toss, kickoff selection, an analog-controlled kickoff and return,
and subsequent defensive scrimmage plays. The first-quarter clock advanced
from 5:00 to 4:58 and beyond. A fresh launch with default dispatch settings
also completed Iowa versus Iowa State loading, a kickoff return, an offensive
snap and handoff, and advancement to second down at 4:50. A rebuilt browser
run also loaded Miami University versus Cincinnati, returned a kickoff, ran
scrimmage plays, processed a fumble, and reached third down at 3:36. At
1440×816 on this machine's Intel Arc A750, observed gameplay was approximately
29–31 game FPS and 98–101% simulation speed, with roughly 6–9 ms CPU cost per
PSP tick. These short-window measurements are local observations, not a promise
for other hardware. The previous 50–60 FPS reading counted browser callbacks;
the corrected counter counts guest display commits. An initial run used `?nochain` to disable
nested generated calls during diagnosis; that option remains available.

A native run completed 6,000 PSP ticks through kickoff and scrimmage play
selection without halting. Its 2,917 display requests were separated by two
Vblanks in 2,831 of 2,916 intervals; longer intervals include loading and host
execution delays. This establishes the title's approximately 29.97 FPS native
cadence, but does not certify compositor timing. In a separate 2,200-tick A/B
test, the idle optimization reduced HLE calls from 23,111,486 to 283,505 while
preserving the two-Vblank cadence. Logs are `build/display-baseline.log`,
`build/idle-accel.log`, and `build/pacing-native.log`. Browser proofs are
`build/ncaa10-browser-gameplay.jpg`, `build/ncaa10-paced-gameplay.jpg`, and the
live handoff capture `build/ncaa10-paced-live-play.jpg`.

Further browser testing used Super Sim to finish the first quarter, returned
to live second-quarter defense (5:00 to 4:44), then used Super Sim to finish
the remaining game. Cincinnati won 17–13; the final score and 3D postgame
scene rendered without a halt. This verifies the simulation, quarter-state,
and postgame paths, not a complete manually played or continuously rendered
match. Proofs: `build/ncaa10-second-quarter.jpg`, `build/ncaa10-sim-final.jpg`,
and `build/ncaa10-postgame.jpg`.

The web AudioWorklet now primes to its 100 ms queue target, clears stale
samples when muted or hidden, and resets its fractional sample position after
starvation. Visibility changes suspend/resume audio and release held keyboard
and touch input. Focus loss also releases the guest controller. Optional
`?profile` diagnostics report producer rate relative to audio consumption.
The rebuilt port loaded UL Lafayette versus Middle Tennessee and completed
kickoff as a touchback. A 22-second foreground menu sample had zero underruns;
loading and later transitions still produced underruns, so audio correctness
and uninterrupted playback remain unresolved. Earlier test instances were
closed before the final stadium test to avoid competing game workers.
Evidence: `build/ncaa10-audio-samples.json`, `build/ncaa10-audio-build.jpg`,
and `build/audio-web-build.log`.

The eight ordinary audio channels now read live guest PCM in 64-sample blocks,
retain independent volumes, and allow one parked producer per channel. Output
handlers distinguish blocking from nonblocking calls, return queued sample
counts after a scheduled wake, validate arguments, allocate automatic channels
from 7 downward, and let an already playing buffer finish after release. Config
and volume imports have real handlers. Null-buffer/rest-length behavior is also
distinguished. These changes follow the PSP behavior documented in
[PPSSPP's hardware test notes](https://github.com/hrydgard/ppsspp/blob/master/docs/sceAudio.md);
the implementation is local. Sub-block timing, interrupt-context waits, and the
separate Output2/SRC two-descriptor path are not certified.

The mixer now quantizes signed volume products with arithmetic shift before
accumulating channels, using a 64-bit intermediate. The old division rounded
negative fractions toward zero (for example, -1 at half gain became 0); the
corrected result is -1. This follows the signed gain operation in
[PPSSPP's audio mixer](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/__sceAudio.cpp).
The public-import/sink regression sweeps all 65,536 signed PCM values with
seven left gains (0, 1, 0x4000, 0x7FFF, 0x8000, 0x8001, 0xFFFF) and complementary
right gains, comparing the complete ordered output against a floating-point
floor/clamp oracle. It also checks that two opposite half-gain sub-LSB channels
quantize separately before summation. The old mixer fails this regression with
a one-unit negative-sample mismatch; the corrected mixer and all eight host
checks pass. Evidence: `build/ncaa-audio-quantization-baseline-test.log` and
`build/ncaa-audio-quantization-host-tests.log`. Native and web builds succeeded
(`build/ncaa-audio-quantization-native-build.log` and
`build/ncaa-audio-quantization-web-build.log`). Exact EBOOT import inspection
confirms NCAA uses ordinary mixer APIs and has no Output2/SRC imports; the
separate legacy path remains open framework work. These tests establish gain
arithmetic, not audible equivalence or underrun-free full matches.
The import list and EBOOT hash are saved in
`build/ncaa-audio-quantization-imports.json`.

A fresh native 3,000-Vblank run of the corrected mixer reached a stadium scene
without halting (`build/ncaa-audio-quantization-native.log` and `.png`). A fresh
browser reload then played Fresno State at San Jose State: manual analog
kickoff and coverage, an encroachment penalty, defensive play selection and a
completed down from first-and-five at the 41 to second-and-nine at the 37.
Instant Replay advanced to the ball carrier's run. The session accumulated one
audio underrun during loading and remained at one through this replay. Settled
snapshots were near 30 FPS and 100% speed at 1440x816, with occasional longer
submission gaps; sustained native pacing is not established. Evidence:
`build/ncaa-audio-quantization-browser-down.jpg`,
`build/ncaa-audio-quantization-browser-replay.jpg` and
`build/ncaa-audio-quantization-browser.log`. This audio arithmetic change does
not resolve the separate intermittent SDSU DAY allocation failure documented
below, certify pixel/audio fidelity to a PSP reference, or complete all Dynasty
features and other modes.

A new native run completed 2,400 ticks through a stadium scene without halting,
with a 39.75-second, 44.1 kHz stereo WAV. The updated recording contained nonzero
audio and no saturated samples in this run; this does not establish audible
equivalence to PSP hardware. Evidence: `build/audio-after.log`,
`build/audio-after.wav`, and `build/audio-dma-wav-stats.json`. Set
`PSPWEB_TRACE_AUDIO=1` for audio-only HLE tracing. The corresponding web build is
logged in `build/audio-dma-web-build.log`.

Dynasty testing found and fixed a save-screen stall: the OSK stub returned
"finished" even before initialization, while NCAA waited for the keyboard to
be idle. The host now implements idle/init/visible/quit/shutdown states,
bounded UTF-16 output and changed/unchanged/canceled results. The browser
offers a text-entry dialog; native headless runs preserve initial text or use
the ASCII `PSPWEB_OSK_TEXT` override. `PSPWEB_OSK_WAIT=1` leaves native entry
pending for host tests. Parameter layout follows
[PSPSDK's OSK header](https://github.com/pspdev/pspsdk/blob/master/src/utility/psputility_osk.h).

An isolated native Air Force Dynasty named `NCAATEST` saved successfully to
`build/dynasty-test-ms/PSP/SAVEDATA/ULUS10440000` and loaded successfully in a
fresh process. Logs: `build/dynasty-save-fixed.log` and
`build/dynasty-native-reload.log`. Before the fix, the same input sequence
stalled on the dimmed file-type screen (`build/dynasty-save.log`). Native test
saves were packaged as `build/ncaa10-dynasty-native-test.zip`; browser import
reported one save with three files. A second Dynasty named `NCAAWEB` was
created through the browser keyboard and saved to slot `ULUS10440001`. After a
page reload, both saves remained available and `NCAAWEB` loaded to the Dynasty
hub. Exported browser data also loaded in a fresh native process. Evidence:
`build/ncaa10-browser-dynasty-reload.jpg`,
`build/dynasty-browser-export-load.log`, and
`build/ncaa10-browser-test-saves.zip`. The browser reload/load session reported
zero audio underruns. This verifies these test saves, not every save type or
complete Dynasty feature coverage.

The reloaded Air Force Dynasty entered its first matchup against Nicholls
State, completed kickoff and defensive scrimmage plays through third down,
and opened game statistics. Instant replay played to its end and the analog
camera control changed the view. Evidence: `build/ncaa10-dynasty-live-defense.jpg`,
`build/ncaa10-dynasty-replay.jpg`, and `build/ncaa10-dynasty-game-stats.jpg`.
Most observed samples were around 30 FPS and 100% simulation speed, but a play
transition briefly showed 18.7 FPS and the session accumulated two audio
underruns (one loading the stadium). This run is not a sustained full-match
frame-rate certification. It did not finish or persist a season result.

Controller polling now keeps the guest neutral while the page is hidden or
unfocused. After focus changes or dialogs, both buttons and the analog stick
must return to neutral before gameplay accepts input again. Previously the
stick could carry movement through a dialog and gamepad polling could undo
the blur handler's neutral input. Synthetic standard-controller tests cover
all mapped buttons, axes/deadzones, disconnects, neutral rearming and focus
transitions; physical Bluetooth hardware has not been tested. The rebuilt
shell is logged in `build/input-web-build.log`. In that rebuilt page, keyboard
selection loaded `NCAAWEB` from the save dialog and subsequent game input
opened Team Schedule successfully (`build/ncaa10-input-update-smoke.jpg`).

The controller HLE now samples input on guest Vblank boundaries (or the
configured 5,555–20,000 microsecond timer), retains 64 historical samples,
and returns stable timestamps between sample boundaries. Positive peeks
return the requested history in chronological order; zero-count requests
write nothing and oversized requests return an error. Sampling mode/cycle
setters validate their arguments and return the previous value, and their
getters are installed. Digital mode centers the analog axes; analog mode
preserves them. These behaviors follow the primary implementation reference
[PPSSPP sceCtrl](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceCtrl.cpp).
The native controller suite exercises these public imports, memory guards,
history wraparound, mode changes, and timer cadence. All three native host
suites and all three web-shell suites passed; the native and web builds
completed (`build/ncaa-controller-test-build.log` and
`build/ncaa-controller-sampling-web-build.log`).

A fresh native process still loaded the `NCAAWK3` recruiting checkpoint
after this change (`build/dynasty-sampled-controller.log` and `.png`).
Opt-in `PSPWEB_TRACE_CTRL=1` confirmed delivery of Square and Circle to
the game's controller wrapper, but Team Overview and Pitch Feedback still
did not visibly open. At that checkpoint ReadBufferPositive still aliased
the peek handler; the consuming/blocking implementation below supersedes
that limitation. Idle-threshold behavior and physical Bluetooth verification
remain unfinished.

Native-only `PSPWEB_TRACE_NCAA_INPUT=1` now traces the game's raw input,
mapper and logical-binding query at 08CBC250, 08CD580C and 08CD682C.
`build/dynasty-input-routing-final.log` confirms that NCAA requests analog
mode, receives stick values 32/224, and maps Square to logical binding 8
and Circle to bindings 9/20 with value 1. Both held and newly pressed bits
reach the game's own input structure. Team Overview still did not visibly
open, so this narrows the investigation beyond host button delivery; it
does not establish whether the remaining behavior is a recompilation
defect or expected game behavior. These opt-in overrides affect generated
unit chaining and must not be used as production performance measurements.
The native executable rebuilt and all three native host suites passed
again (`build/ncaa-input-conversion-build.log` and
`build/ncaa-input-routing-tests-build.log`).

Controller reads now have a separate consuming cursor. They return the
newest available records, retain at most 63 unread records, and wait when
none are available. Each Vblank or configured timer sample wakes one FIFO
reader with one record, including zero-count reads, matching the reference
behavior. Positive and negative APIs share timestamps and axes; negative
buttons invert the complete masked word. Button latches accumulate make,
break, held and released bits across samples; reading clears the latch and
its sample count, while peeking preserves it. Reads reject interrupt/alarm
handler context before waiting. Dispatch-disabled wait validation still
depends on implementing the kernel's dispatch suspension, which remains a
stub. Reference: [PPSSPP sceCtrl](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceCtrl.cpp).

The expanded public-import controller suite verifies consuming/negative
reads, 63-record overflow, three FIFO readers, exact rational Vblank and
5,555-microsecond timer wakeups, zero-count read waits, interrupt error
precedence, latch accumulation/reset and invalid output bounds. All three
native host suites pass (`build/ncaa-controller-read-tests.log`). Native
and web rebuild logs are `build/ncaa-controller-read-build.log` and
`build/ncaa-controller-read-web-build.log`. A fresh native process restored
the saved 2010 preseason (`build/dynasty-controller-read-hub.png` and
matching `.log`). NCAA imports the positive peek rather than the read
API; these changes improve host input fidelity and do not establish a fix
for the recruiting-screen behavior or hardware Bluetooth compatibility.
The rebuilt browser also restored `NCAAYR2` and opened Team Schedule with
keyboard input, showing the opening bye and September 4 Kansas State game.
This fresh session reported zero audio underruns through the schedule
screen, with settled menus around 30 FPS. Evidence:
`build/ncaa10-controller-read-browser-reload.jpg`,
`build/ncaa10-controller-read-browser-schedule.jpg` and
`build/ncaa10-controller-read-browser.log`. This checks save and menu
regression after the host change; it does not repeat the previous live match.

The GE lighting cache now also refreshes when the ambient, diffuse or specular
color registers for any of the four lights change (`0x8F..0x9A`). The old dirty
range stopped at `0x8E`, so a color-only change could reuse the previous light
color. The new GE test executes display lists and reads rendered pixels: red
ambient changes to green, then blue diffuse changes to yellow, independently
for all four lights in both software and GLES3. It failed on the old renderer
(green remained red) and passes with the fix. All four native host suites pass
(`build/ncaa-ge-light-tests.log`). That change repaired cache invalidation;
the subsequent lighting work below adds the previously missing specular path.

A fresh native 6,000-tick run reached live scrimmage after this fix
(`build/ncaa-ge-light-gameplay.png` and `.log`). A separate pre-fix match dump
confirms NCAA enables three directional lights on player geometry
(`build/ncaa-ge-state-before.log`). These different match runs are smoke checks,
not a pixel comparison against PSP hardware. Web build evidence is
`build/ncaa-ge-light-web-build.log`.

The follow-up renderer now calculates specular and powered-diffuse light
components, the view-matrix viewer direction, material specular tracking,
normal reversal, and spotlight cutoff/exponent. Point-light attenuation with
a nonpositive denominator now contributes zero. Light/material products and
each lighting scale use the GE's quantized 8-bit factors; light exponents retain
four mantissa bits and use its piecewise-linear power approximation.

`LIGHTMODE` separates specular RGB from the primary vertex color. Both backends
carry that secondary color through clipping and interpolation, then add it
after the texture operation without changing source alpha. WebGL now cancels
perspective correction for primary and secondary colors, matching the
software backend's interpolation in screen space while preserving perspective
correction for UV. Flat-shaded clipped triangles keep the original final
vertex's primary/secondary colors, even when the near plane removes it.

The GE suite executes display lists and reads pixels in software and GLES3:
combined/separate highlights with a black texture, color-only specular updates,
diffuse-only suppression, material tracking with and without vertex colors,
powered diffuse (including exponent quantization), normal reversal, view-only
updates, point attenuation, spotlight cutoff, source-alpha blending, unequal
clip-w interpolation, and flat shading after near-plane clipping. Regression
evidence: the old renderer produced black instead of `0x804020`
(`build/ncaa-light-components-before-tests.log`); the perspective-color test
produced red 122 instead of approximately 60
(`build/ncaa-light-affine-before-tests.log`); clipped flat color produced red
169 instead of 255 (`build/ncaa-light-clipped-before-tests.log`). All four host
suites pass after these fixes (`build/ncaa-light-pipeline-tests.log`). Web build:
`build/ncaa-light-pipeline-web-build.log`.

Semantics were checked against the [PSPSDK light command encoding](https://github.com/pspdev/pspsdk/blob/master/src/gu/sceGuLight.c)
and PPSSPP's hardware-probe-backed [lighting math](https://github.com/hrydgard/ppsspp/blob/master/GPU/Software/GEMath.h),
[lighting pipeline](https://github.com/hrydgard/ppsspp/blob/master/GPU/Software/Lighting.cpp),
and [exponent/view direction definitions](https://github.com/hrydgard/ppsspp/blob/master/GPU/Common/TransformCommon.h).
These are semantic references; the implementation remains this project's GE
pipeline. The tests establish the listed behaviors, not bit-exact PSP math
across all float24 edge cases, rasterization rules, or a full NCAA reference
comparison. The texture-coordinate work below addresses projection and shade
mapping; mipmapping and other documented accuracy gaps remain open.

The rebuilt web binary was then exercised in a fresh Arizona State/Arizona
match. It completed a manually kicked kickoff, three ASU offensive downs,
a punt and an Arizona fair catch at its own 16. Arizona subsequently completed
a manually snapped rushing play: first-and-23 at its own 3 became
second-and-24 at its own 2, with 4:15 left in the first quarter. Instant Replay
shows Arizona HB #5 carrying the ball and being tackled. Automation delays
before that snap caused three delay-of-game penalties; these are not counted
as offensive plays. Evidence: `build/ncaa-light-pipeline-browser-replay.jpg`,
`build/ncaa-light-pipeline-browser-offense.jpg`,
`build/ncaa-light-pipeline-browser-offense-replay.jpg`, and
`build/ncaa-light-pipeline-browser.log`. Settled samples were approximately
29–30 FPS at 99–101% speed at 1440×816. Eight audio underruns accumulated
over the full session, and occasional long frame gaps remain. This establishes
live gameplay after the renderer changes, not sustained locked frame pacing,
a controlled performance comparison, or proof that NCAA exercised every new
lighting feature covered by the synthetic display-list tests.

Texture-matrix mapping now preserves homogeneous S/T/Q through near-plane
clipping and rasterization in both backends. Division happens after
perspective interpolation, including indexed triangles, rectangles and points.
The normalized-normal source normalizes its input; raw-normal mapping preserves
its length. NCAA's captured player draws use normalized-normal mapping
(`map=0x201`, for example primitive 1976 in `build/ncaa-ge-state-before.log`),
so this repairs a path the title actually exercises. Signed byte/short normals
now decode with divisors 128/32768 in both the direct and morph paths.

Shade mapping independently selects the S/T lights and handles directional
and positional vectors, normal reversal, world transforms, and specular half
vectors from the view direction. It runs with lighting disabled, and refreshes
cached state after light/view changes. Mode 3 aliases ordinary UV mapping;
explicit zero UV scales are preserved. Nonpositive texture Q selects the last
texel on both axes. GLES centers that texel to avoid sampler rounding wrapping
it to zero; the tests cover nearest filtering, not the GE's bilinear edge
precision.

The GE suite reads pixels after real display lists in software and GLES3:
normalized versus raw normal projection, 8/16-bit indices, unequal clip
W and texture Q, near-plane clipping, zero/negative Q, rectangles, points,
through vertices, independent shade-light selectors, disabled lighting,
light/view-only updates, diffuse/specular shade vectors, positional/world
transforms, zero scales, and fixed-point normals with one/two morph frames.
Before the fixes, normalized projection selected texel (10,4) instead of (6,4)
(`build/ncaa-texgen-before-tests.log`), and the minimum signed byte normal
selected (3,4) instead of (4,4)
(`build/ncaa-packed-normal-before-tests.log`). All four host suites pass
(`build/ncaa-texgen-host-tests.log`).

Semantic references are PPSSPP's [vertex transformation and fixed-point decoding](https://github.com/hrydgard/ppsspp/blob/master/GPU/Software/TransformUnit.cpp),
[shade-vector calculation](https://github.com/hrydgard/ppsspp/blob/master/GPU/Software/Lighting.cpp),
and [rectangle/clipping behavior](https://github.com/hrydgard/ppsspp/blob/master/GPU/Software/Clipper.cpp).
These tests do not establish bit-exact float24 arithmetic or a reference PSP
image match. Missing-attribute carry, mipmaps and line primitives still need
work; fog is covered by the follow-up below. Shade mapping is fixture-tested; the recorded NCAA scene
establishes matrix-normal usage, not environment-map usage.

The web rebuild (`build/ncaa-texgen-web-build.log`) reverified ULUS10440 and
the decrypted executable hash. A fresh browser match at 1440×816 loaded
UL Lafayette/Middle Tennessee State, completed the opening kickoff touchback,
then a manually snapped MTSU 16 Power O rushing play. First-and-15 at its own
15 became second-and-18 at its own 12 with 4:54 left in Q1. One earlier
delay-of-game penalty came from time spent inspecting the play-call UI.
Instant Replay shows HB #2 being tackled. Evidence:
`build/ncaa-texgen-browser-result.jpg`, `build/ncaa-texgen-browser-replay.jpg`
and `build/ncaa-texgen-browser.log`. Settled samples were around 30 FPS and
100% speed, with one accumulated audio underrun; longer frame gaps still
occurred. This is a gameplay regression check, not proof of locked frame
pacing or a controlled before/after image comparison.

Fog now uses eye-space depth from view * world after skinning/morphing,
independent of the projection. Both renderers quantize the vertex factor to
0..255, carry it through near-plane clipping, interpolate it in screen space,
and apply the GE's rounded integer RGB blend after texturing and separate
specular. Source alpha is preserved. Through/clear modes bypass fog; rectangles
select the opposite endpoint's factor in each horizontal half. Transform and
fog-register changes refresh the cached depth calculation. Constant software
colors/factors are preserved exactly rather than losing one to floating-point
weight sums.

The fog tests execute display lists and check pixels in software/GLES3 for
orthographic and unequal-W projections, flat/Gouraud shading, factor/color
updates, vertex quantization, saturation/nonfinite factors, view/world changes,
skinning, morphing, clipping, rectangle orientation, points, clear/through
bypasses, enable toggles, alpha tests, and texture/specular ordering.
The initial orthographic test failed on the old software path: it produced
plain red (`0x0000FF`) rather than the expected red/blue blend (`0x7F0080`)
(`build/ncaa-fog-before-tests.log`). All four native host suites pass
(`build/ncaa-fog-host-tests.log`).

The earlier index fixture overstated coverage: value 3 is not one of PSPSDK's
documented index formats and the sequential triangle could also pass as an
unindexed draw. The replacement covers documented 8/16-bit indices, including
values above 255, unused vertex slots, shared/sparse decoding and changed
expected texels on every draw. These cases pass in both backends.

References: [PSPSDK fog commands](https://github.com/pspdev/pspsdk/blob/master/src/gu/sceGuFog.c),
PPSSPP's [vertex fog quantization](https://github.com/hrydgard/ppsspp/blob/master/GPU/Common/TransformCommon.h),
[fog RGB blend](https://github.com/hrydgard/ppsspp/blob/master/GPU/Software/DrawPixel.cpp),
and [rectangle fog selection](https://github.com/hrydgard/ppsspp/blob/master/GPU/Software/Clipper.cpp).
The recorded sunny NCAA draw dump has no fog-enabled primitives. These
fixtures verify the listed semantics; they do not establish fog usage in that
match, bit-exact GE float24 transforms/raster planes, or a PSP image match.

The fog web rebuild completed (`build/ncaa-fog-web-build.log`). A fresh normal
browser start reproduced the existing daytime Fresno State/San Diego State
null-buffer loading failure (`build/ncaa-fog-sdsu-crash.jpg` and `.log`). A
subsequent traced Michigan/San Diego State start loaded, completed a kickoff
return to the 30, and accepted a manually snapped rushing play from the 15
to the 14: second-and-26, 4:52 remaining in Q1, 0–0. Three delay penalties
preceded that snap while the automation inspected screens. Instant Replay
opened and advanced to one second. Evidence: `build/ncaa-fog-browser-result.jpg`,
`build/ncaa-fog-browser-replay.jpg`, and `build/ncaa-fog-browser.log`.
Settled samples were near 30 FPS/100% speed, with 10 accumulated audio
underruns and longer presentation gaps. Disc tracing was enabled in this
successful run; it is functional coverage, not a normal-build performance
measurement or a fix for the earlier loading failure.

Browser regression uncovered a null-buffer disc request while loading San
Diego State against Fresno State, reproduced against New Mexico State. The
failure is still under investigation: the game's DVD reader receives a null
destination for a 128 KiB read before its prefix copy. Evidence is
`build/ncaa-ge-light-sdsu-crash.jpg`, `.log`, and
`build/ncaa-sdsu-disc-trace.log`. Optional `?disctrace` diagnostics log saved
disc requests and null submissions without changing the guest read behavior.
The trace installs host function observers and should be disabled for normal
performance measurements. A subsequent Ole Miss/San Diego State session
loaded successfully, completed a kickoff touchback and a manually snapped
rushing play, then opened its Instant Replay. After input delays caused three
delay penalties, that play advanced from first-and-25 at the 5 to second-and-23
at the 7, with 4:47 left in the first quarter. Evidence:
`build/ncaa-ge-light-browser-replay.jpg`, `build/ncaa-ge-light-browser-run.jpg`
and `build/ncaa-ge-light-browser.log`. Settled play reported around 30 FPS and
100% speed, with four accumulated audio underruns. This diagnostic session
does not establish a fix for the two prior loading failures or a sustained
performance guarantee.

Further `?disctrace` runs reproduced the same null submission in fresh daytime
New Mexico State/San Diego State and Fresno State/San Diego State starts
(`build/ncaa-sdsu-null-producer.log`, `build/ncaa-sdsu-null-heap.log`). The
public DVD wrapper at `0x08C7F660` already receives a zero buffer from
`0x08D69F34`; the queued EA request's buffer is also zero. The read targets
`animdata.dat` (`sce_lbn0x69550_size0x1134A00`) at byte offset `0xA6BA80` for
`0x20000` bytes. Thus the fault precedes host file I/O. The 32 MiB physical
memory and 24 MiB user partition are unchanged. The EA `UserSbrk` reservation
is `0x08EC8F00..0x09EFF300`, with separate title thread stacks.

A cold night start, a warm daytime start, and a subsequent cold daytime start
with temporary allocator observers loaded successfully
(`build/ncaa-sdsu-cold-night.log`, `build/ncaa-sdsu-warm-day.log`,
`build/ncaa-alloc-trace-day-loaded.log`). The temporary allocator observers
were removed: those successes do not prove an allocation failure or establish
a fix. The retained opt-in disc trace includes LBN opens, null-submission
stacks and thread/memory reservations to support a reproducible diagnosis.
Normal runs do not install these observers. The final web rebuild is recorded
in `build/ncaa-final-gameplay-web-build.log`.

The retained trace now samples actual allocator results at `08CD31A4`,
`08CD31CC`, and `08CD31E8`, and observes the synchronous heap-capacity query
at `08D50C3C`. The earlier filesystem callback-entry observer was removed:
its yielded calls did not establish allocation results. A native 6,000-Vblank
probe captured 98 large allocation results and reached the 3D stadium
(`build/ncaa-alloc-result-probe.log` and
`build/ncaa-alloc-result-probe-4000.png`). Another native probe captured 99
capacity-query results and completed 6,000 Vblanks
(`build/ncaa-heap-query-probe.log`). These samples are not an exhaustive
allocation ledger: local generated transfers can bypass return-site observers.

Three fresh daytime Fresno State/San Diego State browser starts reproduced
the failure with increasingly specific diagnostics:
`build/ncaa-alloc-result-sdsu-crash.log`,
`build/ncaa-alloc-capacity-sdsu-crash.log`, and
`build/ncaa-heap-query-sdsu-crash.log`, each with a matching screenshot.
The last trace establishes the immediate allocation failure: the request at
`08CDB4A4` asks for `0x39480` bytes (234,624), while the game's heap query
returns `0x391C0` (233,920), a shortage of `0x2C0` (704) bytes. The query
searches the heap's free list for the largest block. The allocator compares
that result against the request and returns zero before calling its low-level
allocator. The asset-loading path then passes that zero destination onward.
The stream snapshot has zero base/current buffers at offsets 20/44, remaining
size `0x39480`, and file offset `0xA6BA80`. Its read descriptor has a zero
buffer at offset 32 and queues `0x20000` bytes from `animdata.dat`.

This identifies why the observed request receives a null buffer; it does not
explain why the heap becomes too fragmented or constrained in these starts.
An earlier traced daytime start of the same matchup loaded successfully
(`build/ncaa-sdsu-buffer-trace-loaded.jpg` and `.log`). Heap allocation order,
asset lifetimes, and PSP behavior still require comparison before changing
runtime behavior. Physical/user RAM sizes and the guest allocator are unchanged.
The final diagnostic build additionally samples the asset allocation return at
`08CDB4A4` and dumps heap metadata when capacity is insufficient. A separate
native 3,600-Vblank run captured successful `0x2E598` and `0x39480` asset
allocations (`build/ncaa-asset-alloc-probe.log`). Final builds:
`build/ncaa-asset-alloc-web-build.log` and
`build/ncaa-asset-alloc-native-build.log`. All four native host suites pass
(`build/ncaa-asset-alloc-host-tests.log`); those synthetic tests do not cover
this title-specific loading failure or certify full-game accuracy.

A bounded free-list snapshot now confirms fragmentation in another cold
daytime failure (`build/ncaa-heap-free-sdsu-crash.log` and `.jpg`). Before the
`0x39480` allocation, three free blocks contain `0x62960` usable bytes in
total, but the largest contains only `0x391B0`. The list ends at the expected
heap sentinel. The actual asset allocation observer records a zero return.
This distinguishes insufficient contiguous space from insufficient total
space; it does not establish what caused the allocation order.

The subsequent same-team comparison used Fresno State away and San Diego
State home. A cold night start reached the stadium with four free blocks,
`0x6DEB0` total usable bytes and a largest usable block of `0x6A740`. Its
`0x39480` animation buffer was allocated at `0x09E7AB80`. After quitting through
the game menu, a warm daytime start reached the stadium with two free blocks,
`0x62930` total usable bytes and a largest usable block of `0x61AC0`; its
animation buffer was `0x09E83800`. The warm daytime total is just 48 bytes less
than the failed cold daytime total, despite its much larger contiguous block.
Evidence: `build/ncaa-asset-path-night-options.jpg`,
`build/ncaa-asset-path-night-loaded.log` / `.jpg`,
`build/ncaa-asset-path-warm-day-options.jpg`, and
`build/ncaa-asset-path-warm-day-loaded.log` / `.jpg`. The warm log contains the
tail of the preceding night run as well; use its last stadium allocation.

The opt-in allocator trace also reads the decompressed asset path from the
verified `08CC4684` caller frame. It identifies `SDSTAT_Fair.msh` (`0x2E1B0`)
and `SDSTAT_Fair.ord` (`0x5BEB0`), versus `SDSTAT_Night.msh` (`0x1E730`) and
`SDSTAT_Night.ord` (`0x5BE70`). In the failed cold run, an earlier disc read
placed the `0x79C0` loading-loop bank at `0x09E48800`, exactly between the
middle free region and the later stadium `.ord` allocation. The ISO9660
directory identifies LBN `0x60450` as `data/audio/misc/ncaa_lloop_02.bnk`;
LBN `0x60460` is the `0x9340` `ncaa_lloop_03.bnk` used in the successful night
run. The warm daytime bank was placed much lower at `0x09B90F00`.
This makes loading-bank placement/lifetime a concrete next comparison, not
proof of an audio or scheduler defect. No guest allocation order or RAM limit
has been changed. The latest diagnostic additionally samples those bank
allocation sizes and bounded matching frees at `08C8225C`; free-entry and
synchronous free-return messages are distinct, and local generated transfers
can still bypass observers. Builds: `build/ncaa-bank-lifetime-web-build.log`
and `build/ncaa-bank-lifetime-native-build.log`.

The lifetime probe reproduced the cold daytime failure again with the same
three free regions (`build/ncaa-bank-lifetime-day-options.jpg`,
`build/ncaa-bank-lifetime-day-crash.log` / `.jpg`). The bank allocator returned
`0x09E48800` for `0x79C0` bytes to `08B31A9C`. A matching free entry at
`08C8225C` appeared only after the `0x39480` asset request returned zero;
there was no synchronous free-return observation. This is evidence of the
bank's placement and the ordering of a release attempt, not proof that the
free completed or that it should have occurred sooner on a PSP. The latest
trace also reads the bank path from the verified `08B31A30` frame and the
free caller saved by `08CD3B0C`, for the next comparison. Final builds are
`build/ncaa-bank-owner-web-build.log` and
`build/ncaa-bank-owner-native-build.log`. A separate native probe with the
lifetime observer completed 3,600 Vblanks and captured a bank allocation/free
entry (`build/ncaa-bank-lifetime-native.log` / `.ppm`); it used default teams,
so it is not an exact reproduction of the SDSU failure. All four synthetic
host suites pass (`build/ncaa-bank-lifetime-host-tests.log`).
The final native observer probe also completed 3,600 Vblanks
(`build/ncaa-bank-owner-native.log` / `.ppm`). It captured the original sound
loader label `SOUND - SndgLoadFile (audio/misc/NCAA_LLoop_03.bnk)` and a free
owner return address of `08B3348C`, in the `08B3346C` sound-bank unload routine.
That routine releases a sound handle, frees the saved bank buffer, then clears
the corresponding global bank state. This confirms the provenance of those
diagnostic fields; it does not prove why the cold browser placement differs.

After the observer changes, a fresh normal browser start (`?profile`, no
disc/allocator trace) loaded Ohio State at Michigan. A manual kickoff return
reached first-and-10 at the 25 with 4:57 left. Two input delays during testing
produced delay penalties to first-and-20 at the 15. A manually called/snapped
Power O rushing play then reached second-and-16 at the 19 with 4:49 left.
Instant Replay opened and advanced to two seconds. The fresh session reported
zero audio underruns through that replay, with settled samples around 30 FPS
and 100% speed. Evidence: `build/ncaa-bank-normal-kick-return.jpg`,
`build/ncaa-bank-normal-run.jpg`, `build/ncaa-bank-normal-replay.jpg`, and
`build/ncaa-bank-normal-browser.log`. This is a short gameplay smoke check;
it does not resolve the SDSU failure or prove sustained frame pacing/audio
correctness.

The scheduler now services timed waits at the existing heartbeat, every 1,024
outer dispatches, after generated code has returned and flushed its register
cache. Previously that heartbeat only checked the frame deadline: a thread
making no blocking HLE calls could keep a higher-priority delayed thread from
running until the next frame or its own voluntary wait. The new scheduler
regression reproduced that failure before the change, then passed with timer
polling and the existing strict-priority preemption rule. It also checks that
equal/lower-priority threads do not preempt and that the interrupted thread's
registers and stack pointer survive. This uses the existing guest clock, RAM
limits and allocator; it does not introduce a game-specific allocation fix or
measured PSP cycle timing. The checkpoint remains bounded in dispatch count,
not a guaranteed microsecond latency for arbitrarily long generated units.

Runtime diagnostic and deferred-profile dispatch paths also skipped this
heartbeat. They now service it at the same outer boundaries as the normal
path, preserving scheduling and frame deadlines during those investigations.
All seven host checks pass, including the scheduler under normal execution,
`PSPRECOMP_TRACE_ON_ERROR`, and deferred dispatch profiling:
`build/ncaa-scheduler-final-host-tests.log`. Native and web rebuild logs are
`build/ncaa-scheduler-final-native-build.log` and
`build/ncaa-scheduler-final-web-build.log`. The final native diagnostic probe
completed 3,600 Vblanks without halting and rendered a stadium/player scene
(`build/ncaa-scheduler-final-native.log`, `.ppm`, `.png`); its default teams
do not reproduce the SDSU scenario.

A cold guest start with disc tracing enabled loaded Fresno State at San Diego
State, DAY at Qualcomm Stadium, after the scheduler change. The previously
failing `39480` animation allocation succeeded at `09EA7C80`. Total usable
free memory was still `62960`, but the largest free payload was `3D630`, above
the earlier failing `391B0`. The `79C0` loading bank was at `09E44380` and the
stadium `5BEB0` allocation at `09E4BD80`; the bank's free entry still followed
the animation allocation. A manual kickoff reached first-and-10 at the 33
with 4:59 left. The CPU then completed a three-yard rush, reaching
second-and-7 at the 36 with 4:49 left, and Instant Replay advanced to two
seconds. Settled short samples were around 30 FPS / 100% speed; 34 cumulative
audio underruns were reported through replay, most during the traced load.
Evidence: `build/ncaa-scheduler-day-options.jpg`,
`build/ncaa-scheduler-day-loaded.log` / `.jpg`,
`build/ncaa-scheduler-day-kickoff.jpg`,
`build/ncaa-scheduler-day-replay.jpg`, and
`build/ncaa-scheduler-day-gameplay.log`. One successful attempt does not prove
the fragmentation failure is reliably fixed or isolate scheduling from all
other load-order differences. Cold-start repetition and sustained normal-build
audio/frame-pacing checks remain necessary.

The final web build also loaded the same DAY matchup from another cold guest
start with disc tracing disabled (`?profile`). The kickoff ended in a touchback
at the SDSU 20. A delay penalty during input inspection moved the ball to the
15, then a manually selected/snapped rushing play lost two yards, reaching
second-and-17 at the 13 with 4:52 left. Instant Replay advanced to two seconds.
The session reported one cumulative audio underrun through replay. Settled
samples were generally around 30 FPS / 100% speed, but the saved log also has
lower FPS samples and presentation gaps up to about 92 ms during the gameplay
portion. This does not meet a sustained native frame-pacing lock. Evidence:
`build/ncaa-scheduler-normal-day-options.jpg`,
`build/ncaa-scheduler-normal-day-loaded.jpg`,
`build/ncaa-scheduler-normal-kick-return.jpg`,
`build/ncaa-scheduler-normal-run.jpg`,
`build/ncaa-scheduler-normal-replay.jpg`, and
`build/ncaa-scheduler-normal-browser.log`. Both cold guest attempts in this
iteration succeeded; the allocation-order root cause and broader reliability
still require controlled repetition rather than treating these two results as
complete compatibility proof.

Browser catch-up steps now queue presentation immediately after each guest
display commit, before the next Vblank step can enqueue more drawing to the
same framebuffer. Previously a two-step callback queued presentation only at
the end of both steps, allowing the second step's drawing to precede the first
step's committed picture. The extracted former ordering fails an ordered-worker
surface-marker regression (`build/ncaa-presentation-baseline-test.log`). The
new presentation suite verifies both commit phases with inline and native
threaded workers, no-commit cadence, halted batches, rejected submissions,
timing windows and reset behavior. These tests establish host queue ordering;
they are not a PSP reference-image comparison.

`?profile` now labels the existing main-loop measurement `callback gap` and
separately reports completed graphics `submits`, submission interval average/
maximum, intervals over 50 ms (`late`), and presentation queue wait average/
maximum. Queue wait ends when the ordered presentation task begins; interval
timing ends after its GL submission. A mutex protects these statistics across
the guest and graphics threads, and visibility resets exclude the old interval
baseline. These measurements do not observe browser compositor scanout or
physical display refresh. The existing guest clock, two-Vblank NCAA commit
cadence and frame-pacer deadlines are unchanged.

All eight native host checks pass (`build/ncaa-presentation-host-tests.log`).
Native and web builds completed (`build/ncaa-presentation-native-build.log`,
`build/ncaa-presentation-web-build.log`), and a native probe completed 3,600
Vblanks without halting (`build/ncaa-presentation-native.log` / `.ppm`).

Further cold DAY Fresno State/San Diego State loads failed again, both with
normal diagnostics (`build/ncaa-presentation-day-crash.log` / `.jpg`) and with
disc tracing after team browsing (`build/ncaa-presentation-traced-day-crash.log`
/ `.jpg`). The latter requested `39480` with largest usable block `38E30` and
total usable free memory `62960`. Team browsing therefore does not separate
the earlier successful and failing starts. The scheduler and presentation
changes have not reliably fixed this loading failure.

Opt-in allocation tracing now snapshots free lists before the two observed
compressed loading textures (`E57D` and `E68A`) and loading sound banks
(`79C0` / `9340`), and records allocation flags, Vblank count and guest thread.
It also samples high-memory `4450` menu-buffer release entries after loading
starts, with their owner return address. These observers still invoke the
original recompiled functions and do not change guest allocation or lifetime.
Free-entry observations alone do not prove that a yielded free has completed.

The first snapshot build reproduced the DAY failure with texture `E57D` and
bank `9340` (`build/ncaa-loading-layout-day-options.jpg`,
`build/ncaa-loading-layout-day-crash.log` / `.jpg`). Before the texture, a free
region `09E40A90..09E501F0` had size `F760`; after the texture had been read and
before the bank allocation, that same region was fully free again. The bank
then returned `09E46E80` with flags `4`. The previously sampled menu buffer at
`09E50200` bounds this region. The final `39480` request failed with largest
usable payload `391B0`, total `60FE0`. Thus neither compressed texture identity
nor a still-live compressed texture explains the failure by itself; the free
layout preceding the bank allocation needs comparison.

A subsequent native timeline probe completed 3,600 Vblanks without halting
(`build/ncaa-loading-timeline-native.log` / `.ppm`). In `user_main`, texture
`E68A` returned `09E42200` at Vblank 1460, bank `79C0` returned `09E48F00` at
1466, and menu release entries followed at 1473/1479 with owner `08CD8A50`.
The bank release entry followed at 1588 with owner `08B3348C`. These establish
an observed ordering within the same guest thread, not proof of a scheduler
cause or a PSP reference comparison. A separate native snapshot probe reached
Oklahoma/Oklahoma State's stadium introduction
(`build/ncaa-loading-layout-native.log` / `.png`). Both native and web builds
completed; eight host suites pass (`build/ncaa-loading-layout-host-tests.log`).
The final diagnostic build additionally limits menu-release sampling to after
the first observed loading asset. Its native 3,000-Vblank probe also completed
without halting (`build/ncaa-loading-final-native.log` / `.ppm`), with bank
`9340` allocated at Vblank 1462, menu release entries at 1467/1489/1491 and
bank release entry at 1578, all in `user_main`.

The resource-owner trace now resolves the `4450` allocations to a resource
path and slot, using the saved `08CD83A4` frame above the allocator and the
preserved resource/slot at the low-level free entry. Another cold Fresno
State/San Diego State DAY browser start reproduced the failure
(`build/ncaa-resource-owner-day-options.jpg`,
`build/ncaa-resource-owner-day-browser.log`, and its line-separated
`build/ncaa-resource-owner-day-events.log`). Texture `E68A` returned
`09E41B00` at Vblank 8688 and bank `79C0` returned `09E48800` at 8695.
The adjacent menu buffer at `09E50200` belongs to `uis_tmlo.dat`, slot `50`
(the selected San Diego State logo). Its free entry followed at 8702; the
Fresno State logo, slot `1C`, followed at 8703. Trophy artwork
(`uis_trph.dat`, slot `0`) and main-menu backgrounds (`uis_mmbg.dat`, slots
`7`/`5`) followed at 8708/8728. The final `39480` allocation failed at 9412
with capacity `391C0`; the loading-bank free entry followed at 9413.
The reader subsequently halted on its zero destination. This identifies the
owner of the block constraining bank placement, but does not establish an
incorrect guest resource lifetime or resolve the loading failure.

The native owner probe completed 3,000 Vblanks without halting
(`build/ncaa-resource-owner-native.log` / `.ppm`), resolving the corresponding
trophy/background releases. It selected a different default matchup and is
not a native reproduction of the DAY failure. Inspection of host partition
accounting found the expected 256-byte allocation granularity, `4000` loader
reservation, and largest-free-block query; those agree with the corresponding
[PPSSPP kernel memory implementation](https://github.com/hrydgard/ppsspp/blob/master/Core/HLE/sceKernelMemory.cpp).
This source comparison alone does not verify the complete in-game memory
layout against PSP hardware. RAM sizes, guest allocator policy, and resource
lifetimes remain unchanged.

A normal browser session in this iteration loaded UL Lafayette/Louisiana
Tech at night. A manual kick return reached first-and-10 at the 36, 4:58;
the selected scrimmage play accepted a manual snap and analog run, ending
second-and-12 at the 34, 4:49. Instant Replay advanced to two seconds.
Evidence: `build/ncaa-loading-normal-kick-return.jpg`,
`build/ncaa-loading-normal-run.jpg`, `build/ncaa-loading-normal-replay.jpg`,
and `build/ncaa-loading-normal-browser.log`. Settled samples were near 30 FPS
and 100% speed, with zero reported audio underruns through this check. One
replay timing window reported 32 submissions, 33.3 ms average / 45.4 ms maximum
interval and 3.9 ms average / 8.2 ms maximum queue wait. Earlier windows had
longer delays; this short match check does not prove sustained pacing or fix
the independently reproduced daytime load failure.

The earlier normal-build browser run (`?profile`, with disc/allocator tracing
disabled) loaded Louisville/Kentucky, manually selected and kicked Kickoff
Middle with K/I, then selected Dime / Crash Gold for a scrimmage play.
Moving the selected defender toward the line before the snap produced a
five-yard defensive penalty: first-and-10 at the 49 became first-and-5 at
the 44, with 4:53 left in the first quarter. The CPU pass ran to completion
and Instant Replay opened and advanced to two seconds. Evidence:
`build/ncaa-final-browser-live.jpg`, `build/ncaa-final-browser-replay.jpg`,
`build/ncaa-final-browser.log`. Settled samples were around 30 FPS / 100%
speed, with one accumulated audio underrun. This verifies actual interactive
play and replay in the final build; it does not certify every control action,
an entire manual match, or sustained frame-rate/audio behavior.

The rebuilt web port completed all ten repetitions of Spring Drills →
Offense → Rushing Attack with Akron on Varsity difficulty, reaching its
results table with a score of -10. Manual keyboard inputs started attempts;
the game executed runs, tackles, scoring and repetition resets. D-pad and
analog-key probes were exercised, but this run does not isolate their
movement response or certify every ball-carrier action. The PSP icon after
difficulty confirmation is the player-assignment step and requires another
Cross confirmation before loading. Evidence:
`build/ncaa10-controller-sampled-rushing-live.jpg`,
`build/ncaa10-controller-sampled-rushing-complete.jpg`, and
`build/ncaa10-controller-sampled-rushing-browser.log`.
Most settled drill samples were 29.7–30.3 FPS near 100% simulation speed;
loading, transitions and browser callback stalls still dipped below that.
The session accumulated seven audio underruns by the results table. This
verifies one completed drill, not full Spring Drills coverage or a sustained
frame-rate/audio guarantee.

Frame diagnostics now distinguish CPU, graphics-wait, and idle budget hits,
and report the longest guest step and browser callback gap in each sample.
With `?profile`, intervals exceeding a 25 ms step or 50 ms callback gap are
retained as `[timing]` lines in the page log. Native runs can set
`PSPWEB_TRACE_FRAME_BUDGET=1` to include the active guest PC and return address.
In single-threaded native rendering, graphics work shares the CPU budget;
a CPU hit alone does not establish that guest code caused the delay.

The graphics worker now uses Emscripten's immediate scheduling mode when the
worker exposes native `scheduler.postTask`, avoiding nested timer clamping.
Other browsers retain the timer path; `?getimer` forces that path for comparison.
PSP simulation deadlines and the game's two-Vblank presentation cadence are
unchanged. Visibility events explicitly reset the frame pacer and timing
sample, including when requestAnimationFrame stops entirely in a hidden tab.
The web audio regression covers this reset and its startup guard.

The task-scheduled build reached Air Force Dynasty kickoff selection with
29.8–30.2 FPS and 99–101% simulation-speed samples. Its audio underrun count
remained at three after stadium loading. Loading still showed a 116.5 ms
callback gap and a 38.7 ms guest step. These samples and the earlier timer
samples cover different scenes, so they are not a controlled performance
comparison. Evidence: `build/ncaa10-ge-timer-samples.json`,
`build/ncaa10-ge-task-samples.json`, and `build/ncaa10-ge-task-kickoff.jpg`.
The final default build is logged in `build/ge-task-pacing-web-build.log`.
All three web-shell regression scripts and three native host suites pass.

The final default build also loaded Texas versus Oklahoma, returned kickoff
to the 32-yard line at 4:57, and executed a manually snapped scrimmage play
through second down at 4:51. A subsequent manually targeted pass was
intercepted, then a roughing-the-passer penalty restored Oklahoma possession
at its own 17 with 4:20 remaining. The in-game play clock was temporarily
disabled for the scrimmage input checks because UI inspection delays had
caused delay-of-game penalties; it was restored to On afterward. PSP timing
was unchanged. The live-play capture reports 30 FPS, 100% speed and zero
audio underruns. This is a short gameplay check, not a full-match test.
Evidence: `build/ncaa10-task-default-live-play.jpg` and
`build/ncaa10-task-default-pass.jpg`. The native build also rebuilt
successfully (`build/final-native-check.log`).

Dynasty week progression now has a verified save round trip. Loading the
baseline `NCAAWEB`, choosing week two and confirming simulation produced an
Air Force 27–0 win against Nicholls State and a 1–0 record in the browser.
The Program Standards screen also allowed FS #29's suspension to change from
three games to four, leaving 51 discipline points. These states were saved as
`NCAAPRG` in test slot `ULUS10440000`, replacing the backed-up `NCAATEST` fixture.
The `NCAAWEB` baseline in slot 001 stayed byte-identical in DATA.BIN, PARAM.SFO
and ICON0.PNG. The original fixtures remain in
`build/ncaa10-browser-test-saves.zip`.

After a page reload, `NCAAPRG` appeared in the save list and loaded with the
same 27–0 result, 1–0 record, four-game suspension and 51 remaining points.
Exported data loaded with the same values in fresh native processes. Evidence:
`build/ncaa10-dynasty-progress-browser-reload.jpg`,
`build/ncaa10-dynasty-discipline-browser-reload.jpg`,
`build/dynasty-progress-native-reload.log`, and
`build/dynasty-progress-native-discipline.log`. The exported checkpoint is
`build/ncaa10-dynasty-progressed-saves.zip`; file sizes and SHA-256 hashes are
recorded in `build/ncaa10-dynasty-progressed-save-manifest.json`.
The progressed Dynasty's DATA.BIN SHA-256 is
`82d346ff1f152f67349cdb643fc73c193a9de789c9860cf723ddcddcfccf7706`.

Native exploratory simulations produced different scores and sometimes a
player-infraction prompt, so their fixed input schedules are not reliable
unattended save recipes. The browser checkpoint above was checked visually
at each branch. The save/reload menu samples were around 30 FPS with zero
audio underruns, but season simulation had 12.9–16.8 FPS samples and long
guest steps. This verifies these state transitions, not full-season speed,
recruiting, offseason behavior, or complete Dynasty accuracy.

In-season recruiting allocation also survived a save/export/fresh-process
round trip. The browser assigned 20% to Daniel Smith and 12% to Alex Massey,
leaving 17 of 25 points; prospect information opened and included feedback
about the Nicholls State result. Saving as `NCAAREC` replaced test slot 000,
while the earlier `NCAAPRG` checkpoint remains in its exported ZIP. A fresh
native process loaded the `NCAAREC` export and displayed the same 20%, 12%
and 17-point values (`build/dynasty-recruiting-native-reload.log` and `.png`).
The checkpoint is `build/ncaa10-dynasty-recruiting-saves.zip`, with hashes in
`build/ncaa10-dynasty-recruiting-save-manifest.json`; its DATA.BIN SHA-256 is
`cf7c1595dc77e0db18ff5c1b6dd1805613ca51843fc419c8122e200f839b1832`.
Browser evidence is `build/ncaa10-dynasty-recruiting-allocation.jpg`.
Reloading the browser also restored all three values; evidence is
`build/ncaa10-dynasty-recruiting-browser-reload.jpg`.

Recruiting Team Overview and Pitch Feedback have not been verified. The
attempted Circle/Square board inputs left the visible board unchanged in the
browser and in native short/long button probes; this does not establish a
game defect without a reference comparison. Commitments, signing, and
offseason recruiting remain untested.

Advancing the restored recruiting checkpoint into week three simulated an
Air Force 21–59 loss at Minnesota, updating its record to 1–1. Daniel Smith's
board status changed to "Air Force passes the first cut" with an increased
interest indicator. His prospect information showed an unofficial-visit
reaction and updated school-interest arrows. The board also changed its
allocations to 52% / 0% / 0%, leaving 12 points, without manual allocation
input after the simulation. That redistribution is an observed result,
not a verified match to PSP behavior. Team Overview and the separate Pitch
Feedback command still did not visibly open.

This state was saved as `NCAAWK3` in test slot 000 and exported to
`build/ncaa10-dynasty-week-three-saves.zip`. Fresh native processes loaded
the same schedule, first-cut status, allocations, and prospect information:
`build/dynasty-week-three-native-schedule.png`,
`build/dynasty-week-three-native-board.png`, and
`build/dynasty-week-three-native-reload.png` (with matching `.log` files).
Browser captures are `build/ncaa10-dynasty-week-two-result.jpg`,
`build/ncaa10-dynasty-recruiting-week-three.jpg`, and
`build/ncaa10-dynasty-recruiting-week-three-prospect.jpg`.
The DATA.BIN SHA-256 is
`d09900df3028dca722a6b82557f607efdca05ba0ea33233ea22bb4adb777c19e`;
all six exported files are listed in
`build/ncaa10-dynasty-week-three-save-manifest.json`. Slot 001 remains
byte-identical to the original `NCAAWEB`; both earlier progressed
checkpoints remain in their exported ZIPs. The current browser slot 000
was `NCAAWK3`. A subsequent browser restart also restored the 1–1 record,
21–59 Minnesota result, 52% / 0% / 0% allocations, 12 remaining points,
first-cut status and Daniel Smith's prospect information. Evidence:
`build/ncaa10-dynasty-week-three-browser-schedule-reload.jpg`,
`build/ncaa10-dynasty-week-three-browser-board-reload.jpg`, and
`build/ncaa10-dynasty-week-three-browser-prospect-reload.jpg`.
That reload session reported zero audio underruns; this does not establish
uninterrupted gameplay audio or a sustained frame-rate lock.

During this weekly simulation the browser reported 11.9–20.0 FPS,
98–102% simulation speed, and zero audio underruns, with CPU budget hits.
Menus returned to around 30 FPS. These samples do not establish a sustained
30 FPS lock or explain how simulation screen pacing compares to a PSP.

The browser subsequently simulated weeks three through seven and reached
the week-eight Air Force–Utah side-selection screen. Air Force was 1–6
(conference 0–4), with losses of 17–64 to New Mexico, 10–49 to San Diego
State, 21–28 to Navy, 14–24 to TCU and 14–38 to Wyoming. Recruiting reached
the top-three-school stage with zero points left and zero allocation to
all three prospects; Alex Massey's remaining schools were Colorado State,
Hawai'i and LSU. No manual recruiting allocations were made during this
advance. These are observed game results, not PSP-reference accuracy
certification. Browser evidence is in
`build/ncaa10-dynasty-week-eight-schedule.jpg`,
`build/ncaa10-dynasty-week-eight-recruiting.jpg`, and
`build/ncaa10-dynasty-week-eight-prospect.jpg`.

`NCAAWK8` was saved in test slot 000, backed up in
`build/ncaa10-dynasty-week-eight-saves.zip`; a fresh native process restored
the same 1–6 schedule (`build/dynasty-week-eight-native-schedule.png` and
`.log`). The DATA.BIN SHA-256 is
`c5c8115cacb3f06b62c461228711fd236e45ced013a1ddb240b08fec2a8c405c`.
The six-file manifest is
`build/ncaa10-dynasty-week-eight-save-manifest.json`; all three baseline
slot 001 files remain byte-identical. The browser log for the advance is
`build/ncaa10-dynasty-week-eight-browser.log`. Simulation samples ranged
down to 9 FPS with CPU budget hits; menus returned near 30 FPS and audio
underruns remained zero. A browser restart of this newer checkpoint has
not yet been tested.

Continuing from week eight, Simulate Season completed the regular season
at 1–11 (conference 0–8). Conference-week and all-bowls simulation then
reached the Heisman voting table, a new NCAA season-record notification,
and the offseason schedule. Florida QB #15 won the displayed Heisman
vote; the offseason cover reported Florida's championship. Evidence:
`build/ncaa10-dynasty-regular-season-complete.jpg`,
`build/ncaa10-dynasty-heisman.jpg`,
`build/ncaa10-dynasty-season-record.jpg`, and
`build/ncaa10-dynasty-offseason-schedule.jpg`.

School Budgets accepted a one-point transfer from recruiting to training,
changing 35/32/33 to 34/33/33 while preserving zero unallocated points.
Continuing reached Players Leaving, with graduating seniors and an
accessible player-information panel (C #65: two pancakes, zero sacks
allowed in 2009). Evidence:
`build/ncaa10-dynasty-offseason-budgets.jpg` and
`build/ncaa10-dynasty-offseason-players-leaving.jpg`.
Heavy season and offseason processing caused audio starvation: the
session had 30 underruns by Players Leaving, including a measured
163 ms guest step while preparing recruiting budgets. This is additional
feature coverage, not full Dynasty correctness, native pacing, or audio
certification. The saved `NCAAWK8` export predates these offseason steps.

Incoming Transfers offered Texas SS #5 and accepted the displayed default.
Recruiting Central opened the state map and Colorado prospect report.
Scouting Marcus McDougal cost three points (25 → 22), revealed average
potential/discipline, and corrected his 40 time from 4.28 to 4.39, bench
from 300 to 285, squat from 360 to 350, and vertical from 41.0 to 39.2.
A head-coach phone call with the Academics pitch spent two further points
and marked him targeted, reducing scholarships from 24 to 23. The first
week completed with no commitments; Current Targets retained McDougal
and also showed CPU-added targets. Pitch Feedback still did not visibly
open on that screen. Evidence:
`build/ncaa10-dynasty-offseason-transfer.jpg`,
`build/ncaa10-dynasty-offseason-scout.jpg`, and
`build/ncaa10-dynasty-offseason-target.jpg`.

An artificial 1 ms native budget probe of season simulation reached the
advancing-week screen without halting. Its 147 post-confirmation budget
hits frequently stopped at callers of 0886EE4C, which runs game callbacks;
this is not a wall-time profile or justification to skip that routine.
Artifacts: `build/dynasty-season-budget-probe.log` and `.png`.

Simulate All Weeks completed the remaining offseason recruiting and
Finalize Recruiting reached Position Changes. Editing QB #12 displayed
the minimum-QB warning and previewed HB overall 59 versus QB overall 74;
the edit was canceled, so no position change was committed. Training
Results then populated: FB #40 gained six to overall 62, and QB #10
gained four from 80 to 84. Evidence:
`build/ncaa10-dynasty-offseason-position-preview.jpg` and
`build/ncaa10-dynasty-offseason-training.jpg`. The session reached 117
audio underruns after these heavy steps, with a 385.5 ms maximum guest
step recorded. Feature progression works here, but latency and audio
starvation remain reproducible problems requiring further diagnosis.

Walk-on processing completed and Cut Players showed a 53-player roster;
no manual player cuts were made. Set Depth Chart accepted QB #11 in slot
one and moved QB #10 to slot two. Continuing then began generating the
next season's schedule. Evidence:
`build/ncaa10-dynasty-offseason-cut-players.jpg` and
`build/ncaa10-dynasty-offseason-depth-swap.jpg`.

The next season finished generating and returned to 2010 Preseason
Options (`build/ncaa10-dynasty-second-season.jpg`). This completes one
simulated season-to-offseason-to-next-preseason cycle without a guest
halt; it does not exhaust every Dynasty branch or certify PSP accuracy.
The session had 126 audio underruns at the new preseason hub.

Saved test slot 000 is now `NCAAYR2`, backed up in
`build/ncaa10-dynasty-year-two-saves.zip` with its six-file manifest in
`build/ncaa10-dynasty-year-two-save-manifest.json`. DATA.BIN SHA-256:
`39bcc30560a79947e2f60c68f9ae1273e9e1802f9806ecb3b73b0828235c6407`.
All three baseline `NCAAWEB` slot 001 files remain byte-identical.
Fresh native processes restored both the 2010 preseason hub and the
changed depth chart: QB #11 starts at overall 74 and QB #10 is second at
overall 84. Evidence: `build/dynasty-year-two-native-hub.png` and
`build/dynasty-year-two-native-depth.png`, each with a matching `.log`.
The first depth-chart probe ended while loading; extending it to 6,500
Vblanks reached the populated screen. The full browser session log is
`build/ncaa10-dynasty-season-cycle-browser.log`. A fresh browser restart
also restored `NCAAYR2` to the 2010 preseason hub, with zero audio
underruns through restoration. Evidence:
`build/ncaa10-dynasty-year-two-browser-reload.jpg`.

From that browser reload, advancing the opening bye week reached the
September 4 Kansas State game. Program Standards opened the academic
infraction for freshman punter Jeff Harris; its three-game suspension
dialog returned to the roster and game setup. The matchup loading screen
listed Harris among Players Out. Evidence:
`build/ncaa10-dynasty-year-two-discipline.jpg`.
The regenerated roster loaded into the 3D stadium and accepted a manual
analog kickoff, defensive play calls, and defender movement. Play advanced
from first-and-ten at the 41 to third-and-thirteen at the 38. Settled play
was around 30 FPS and 100% speed, with one audio underrun since restart.
Evidence: `build/ncaa10-dynasty-year-two-live-kickoff.jpg` and
`build/ncaa10-dynasty-year-two-live-defense.jpg`. This is additional live
gameplay coverage after a season rollover, not a complete manual match.
The following offensive possession used the saved starter QB #11. HB #17
recorded one carry for ten yards; the subsequent Spacing pass play ended
in a seven-yard sack before a pass attempt was recorded. Individual Stats
confirmed both entries (`build/ncaa10-dynasty-year-two-live-stats.jpg`).
Instant Replay advanced and rotated correctly around QB #11 with the ball
(`build/ncaa10-dynasty-year-two-gameplay-replay.jpg`). The fresh-session
log is `build/ncaa10-dynasty-year-two-browser.log`; the browser was left
paused in this live match before the later controller rebuild. The archived `NCAAYR2` preseason save was not
overwritten by these in-match actions.

Compiler and host regressions use synthetic data, without game assets:

```powershell
cmake -S PSPRecomp -B build/tools -DPSPRECOMP_BUILD_TESTS=ON
cmake --build build/tools --target psprecomp_tests
New-Item -ItemType Directory -Force build/test-tmp | Out-Null
# Use a workspace temporary folder if the Windows sandbox temp directory is restricted.
$env:TEMP = "$PWD/build/test-tmp"
$env:TMP = $env:TEMP
ctest --test-dir build/tools --output-on-failure
python scripts/test_ncaa_codegen.py
# Use an installed Node.js executable (the Emscripten SDK includes one).
node scripts/test_web_audio.mjs
node scripts/test_web_osk.mjs
node scripts/test_web_input.mjs
# Native HLE/scheduler tests; use the existing native build's toolchain/GL setup.
cmake -S PSPRecomp -B build/native-ncaa10 -DPSPWEB_HOST_TESTS=ON
cmake --build build/native-ncaa10 --target pspweb_audio_tests pspweb_utility_tests pspweb_controller_tests pspweb_ge_tests pspweb_scheduler_tests pspweb_presentation_tests
ctest --test-dir build/native-ncaa10/profiles/web -R 'pspweb_.*_tests' --output-on-failure
```

Both the framework suite and the compiled synthetic regressions pass. The
latter execute VI2S packing and destination masks, relocated JAL/delay-slot/
return behavior at two load addresses, start-thread export parsing/defaults,
manifests with CRLF and doubled carriage returns, display latch/error behavior,
the guarded NCAA wait predicate, and pacing across simulated 30/60/120/144 Hz
host refresh rates, stalled/resumed clocks, and long-run rational guest time.
The web-shell regression executes the actual AudioWorklet at 22.05/44.1/48/96
kHz output, checking stereo chunk continuity, ten seconds of rate conversion,
queue limits, stale-sample reset, starvation recovery, and focus-loss input
release with the WebAssembly startup guard.
The native audio suite executes the installed HLE and scheduler with synthetic
PCM. It checks full-range signed gain quantization against an independent
oracle, per-channel rounding before summation, live buffer edits and volume
changes, mono duplication, stereo mix saturation, released-buffer completion, allocation/error precedence, null
buffers, two-thread contention, and the sample count returned after blocking.
The utility suite verifies OSK lifecycle, repeated initialization rejection,
output bounds, cancellation, unchanged input and surrogate-safe truncation.
The controller suite verifies bounded peeks, sampled history and timestamps,
digital/analog modes, reserved output bytes, and sampling timer cadence.
The scheduler suite verifies timed high-priority preemption during a busy
guest thread, equal/lower-priority deferral and register restoration. It runs
in normal, runtime diagnostic and deferred dispatch profiling configurations.
The presentation suite checks committed-surface queue ordering across catch-up
steps, cadence, halt behavior and submission timing/reset with inline and
native threaded graphics workers.
The GE suite renders display lists to RGBA8888 and checks pixels in both the
software and GLES3 backends, including the lighting and color-pipeline cases
documented above. On Windows it uses the native build's ANGLE EGL/GLES DLLs.
The browser OSK regression executes the shell dialog with a minimal DOM and
WebAssembly bridge, checking UTF-16 submission, cancellation, focus return,
input release, and suppression of a controller button held while opening it.

This is a gameplay milestone, not a full-game compatibility certification.
Full matches, every stadium/team, Dynasty/Road to Glory, save round trips,
and complete audio correctness have not been exhaustively tested. Intro
movies are skipped, network play remains offline, and encrypted PSP saves
are not supported by the existing host. The browser audio queue reported
underruns during loading and early gameplay; audible accuracy and underrun
recovery still need work. Sustained full-match compatibility, rendering
comparison against a PSP reference, performance under heavier scenes, native
input/audio behavior, and measured presentation pacing remain open work.

The 2026-10-14 fixed-plane checkpoint adds measured GPU UV/fog setup, shared rectangle planes, fragment arithmetic and four-pixel span LOD. Full SDSU/GT replay RGB MAE is 0.008749 / 0.013225 against pinned `61e60ee6ff`; five early SDSU cases and prefix-40 raw RGB match exactly. Eleven native suites include GPU arithmetic/metadata-lifetime tests. Current package: `build/static-ncaa10-fixed-planes-final`. See [handoff](../HANDOFF.md#fixed-point-uvfog-planes-and-span-lod-checkpoint-2026-10-14) for performance and remaining coverage; this is not a new full-match/audio/controller verification.
