# Audio and frame pacing audit — 2026-10-20

The browser audio path and frame pacer were audited and polished. The local
Madden build was rebuilt and booted using the supplied ULUS10441 ISO.

## Changes

- Replaced retained PCM chunks and render-time array shifting with a fixed
  128 KiB stereo ring. Overflow discards exactly the excess source frames,
  retaining the existing 250 ms latency ceiling and 100 ms startup reserve.
- Added 3 ms transitions at startup, starvation recovery and overflow cuts.
  Starvation clears stale resampling phase and drift correction. Normal
  playback keeps its interpolated PCM unchanged.
- Mute ramps over 5 ms; unmute discards old queued audio and primes fresh PCM.
- Audio initialization resumes during the first gesture, retries after setup
  failures, handles closed/interrupted contexts, releases module blob URLs and
  suspends if the page becomes hidden while the worklet loads.
- Moderate frame stalls allow at most two catch-up steps, then discard excess
  deadline debt. Normal 30–240 Hz display cadence remains tied to PSP Vblank.
- Audio reports run once per device second. Their rate calculations use wall
  timestamps taken in the worklet, so reports delivered together after a main
  thread stall no longer imply impossible production/render rates.

## Validation

- `node scripts/test_web_audio.mjs`: 22.05/44.1/48/96 kHz resampling, chunk
  boundaries, nonconstant stereo ring wrap, drift, bounded overflow, recovery
  continuity, delayed reports and context retry/visibility lifecycle.
- `profile/tests/frame_pacer.cpp`: standalone C++ assertions covering one minute
  at 30/60/75/120/144/240 Hz, moderate/long stalls, invalid clocks and reset.
  It is also registered with CTest when host tests are enabled.
- `node scripts/test_web_audio_browser.mjs`: passed in headless Edge at 48 kHz;
  checked actual non-silent graph output, stats, starvation, mute and unmute.
  The script requires Playwright; `NODE_PATH` and `PSPWEB_BROWSER_EXECUTABLE`
  can select a local installation and Chromium executable.
- Existing web input, saves, persistence and OSK checks passed. Static package
  and Pages player suites passed (15 Python tests). WebAssembly build passed.

## Game smoke check and remaining performance work

A 20-second headless Edge sample reached the title screen, generated nonzero
PCM (peak 0.303, RMS 0.025) and reported no JavaScript errors. Several title
screen intervals ran around 30 FPS / 100% guest speed, with approximately
44.1 kHz input into a 48 kHz audio graph and no queue overruns.

Startup and occasional callback stalls remain. This sample recorded a maximum
guest callback cost of about 1.30 seconds and six audio starvation events by
the final report. The fixes smooth audio recovery and bound subsequent frame
catch-up; they do not eliminate the stalls themselves. The trace alone does
not establish whether compilation, guest execution or browser scheduling
caused each pause. This was a boot/title-screen check, not an extended gameplay
or listening assessment.

Local evidence (ignored build outputs): `build/audio-polish-game-audit.json`
and `build/audio-polish-boot.png`. That trace predates the final report timestamp
fix; its extreme instantaneous render-rate readings are diagnostic artifacts.

The final package is `build/static-madden10-audio-polish-final`. Serve it from
the repository root with:

```powershell
python scripts/serve.py build/static-madden10-audio-polish-final 8622
```

Open `http://127.0.0.1:8622/` and select the local ISO. Add `?profile` to inspect
frame timing and audio queue diagnostics.
