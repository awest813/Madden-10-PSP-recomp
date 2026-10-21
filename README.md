# Madden NFL 10 PSP Web Recomp

A browser port of **Madden NFL 10 for PSP (USA, ULUS10441)**, built on [PSP Web Recomp](https://github.com/snuri00/psp-web-recomp). The game's MIPS executable is translated ahead of time into C++, compiled to WebAssembly, and linked with a PSP runtime that provides graphics, audio, input, file access, and saves.

**[Open the player](https://awest813.github.io/Madden-10-PSP-recomp/)** · [Build locally](#build-madden-from-a-fresh-checkout) · [Controls](#controls) · [Technical documentation](#documentation)

Bring your own matching, uncompressed PSP ISO. The player reads requested ranges from the selected file on your device; it does not upload your ISO. This is a title-specific recompilation, and selecting a different PSP game does not make it compatible.

## Current status

The Madden build boots to the title screen in the browser. The latest documented smoke check observed nonzero audio output and intervals around 30 FPS at 100% guest speed. Startup and occasional callback stalls still cause audio starvation. That check covered boot and the title screen; extended gameplay, full matches, and all game modes remain unverified. See the [audio and frame pacing audit](docs/AUDIO_STUTTER_AUDIT.md) for measurements and their limits.

The hosted player uses a **pinned release** recorded in [docs/PAGES_PLAYER.json](docs/PAGES_PLAYER.json). Source changes do not update that runtime until a new package is published and the pin is changed, so a local build can contain fixes absent from the hosted player.

This fork also retains NCAA Football 10 support and its development notes. NCAA gameplay evidence and the inherited God of War demo are evidence for those titles, not Madden compatibility or performance guarantees.

## Play in the browser

1. Open the [player](https://awest813.github.io/Madden-10-PSP-recomp/) in a browser with WebAssembly, WebGL2, and service workers enabled.
2. Select your **Madden NFL 10 PSP USA ISO (ULUS10441)**. The picker checks the disc identity and executable revision. ZIP, CSO, and ZSO files are unsupported in the picker.
3. Keep the page active while it loads, then press **Enter** for Start. Interact with the page to enable browser audio.

Select the ISO again after reloading. The hosted page supplies the compiled runtime and boot preload; players do not need a compiler or a separate disc server.

### Controls

| Keyboard | PSP input |
| --- | --- |
| Enter | Start |
| Z / X | Cross / Circle |
| A / S | Square / Triangle |
| Arrow keys | D-pad |
| I / J / K / L | Analog up / left / down / right |
| Q / W | L / R shoulder buttons |
| Shift | Select |
| F | Toggle fullscreen |

The web shell also supports gamepads and on-screen touch controls. Physical controller and device compatibility require testing on your hardware.

### Saves

Saves use browser storage for the current site address. Changing the domain, port, browser, or browser profile creates a separate save location. Clearing site data can remove saves.

- Use **Export saves** to keep a ZIP backup before changing addresses or clearing storage.
- Use **Import saves** to restore a ZIP containing `PSP/SAVEDATA/<slot>/` or `<slot>/`. Imports replace matching slots; export a backup first.
- Saves from a PSP or PPSSPP may be encrypted. Importing a ZIP does not decrypt them.

If the page reports a storage failure, export any new in-memory progress before reloading. A failed initial restore disables automatic writeback for that session to protect existing stored data.

## Build Madden from a fresh checkout

Run all commands from the repository root containing `scripts/` and `profile/`. In the original downloaded workspace, this is the inner `psp-web-recomp-main` folder.

### Requirements

- Git and Git Bash for the setup script on Windows.
- Python 3, CMake, Ninja, and a C++20 compiler on `PATH`.
- An installed and activated Emscripten SDK; setup selects version **6.0.10**.
- Your matching Madden ISO and a **decrypted ELF executable** from that disc.
- Disk space for the extracted disc, raw streaming image, generated C++, and build outputs.

The Windows PowerShell script is the Madden build entry point. Native diagnostic builds additionally require EGL/OpenGL ES libraries; windowed native builds require SDL2.

### 1. Fetch the framework and SDK

In Git Bash:

```bash
git clone https://github.com/awest813/Madden-10-PSP-recomp.git
cd Madden-10-PSP-recomp
bash scripts/setup.sh
```

Setup fetches the pinned PSPRecomp revision, applies the patches in `patches/`, connects the web profile, installs Emscripten under `tools/emsdk`, and builds the native recompiler tools. If you already have an SDK, use `bash scripts/setup.sh --no-emsdk` and pass its location to the build command.

### 2. Prepare the disc and decrypted executable

In PowerShell, from the same repository root:

```powershell
python scripts/extract_iso.py "C:/path/to/Madden NFL 10.iso" games/madden10/root/disc

# Copy your decrypted ELF dump, not the encrypted retail EBOOT.
Copy-Item -LiteralPath "C:/path/to/decrypted/EBOOT.BIN" -Destination games/madden10/root/EBOOT.BIN

python scripts/verify_madden10.py games/madden10/root
```

The build script does **not** decrypt the executable. Obtain a decrypted dump using PPSSPP's developer tools or a compatible decrypter. The optional `scripts/decrypt.sh` wrapper calls the tool named by `PSP_DECRYPT` as `tool <input> <output>`; it does not install a decrypter.

Verification requires disc ID `ULUS10441` and this decrypted executable SHA-256:

```text
5c5064c142139b9bd9bb5fff7ab8421f8364cc188a86c2b6eca06ffd69735da3
```

A different region or executable revision needs its own verified port and generated sources.

### 3. Compile and launch

In PowerShell:

```powershell
./scripts/build_madden_web.ps1 `
  -Iso "C:/path/to/Madden NFL 10.iso" `
  -Emsdk ./tools/emsdk `
  -Serve
```

Open **http://127.0.0.1:8622/** and select the matching ISO. The command builds the runtime, prepares a fresh timestamped static package, and serves its ISO picker.

| Option | Purpose |
| --- | --- |
| `-Generate` | Regenerate the executable and EA audio-bank C++ sources |
| `-Jobs 4` | Set parallel build jobs; default is 4 |
| `-Emsdk <path>` | Select an SDK; the script otherwise defaults to `$env:USERPROFILE/emsdk` |
| `-StaticOutput build/my-madden-player` | Package to a new directory under `build/` |
| `-Serve` | Package and start the local server |
| `-Port 8622` | Set the local server port; default is 8622 |

Without `-Serve` or `-StaticOutput`, the script only builds. Missing generated sources are created automatically. The build also prepares the disc manifest and raw UMD backing image. Generated sources and game-derived outputs stay in ignored local directories.

### Launch an existing build

Create a new ISO-picker package from an existing web build:

```powershell
python scripts/prepare_static_madden.py --output build/static-madden10
python scripts/serve.py build/static-madden10 8622
```

The packager refuses to overwrite an existing output directory. Choose a new name for subsequent packages.

For development with the extracted disc streamed by the local server:

```powershell
python scripts/serve.py build/web-madden10/profiles/web 8622 --mount /disc=games/madden10/root/disc
```

Open the HTTP address rather than opening `index.html` directly. Add `?profile` to inspect frame timing and audio queue diagnostics.

## Hosting and releases

The static package includes title-specific WebAssembly and a verified decrypted boot preload. It omits the full ISO, extracted disc assets, and saves; the player supplies the ISO locally. These packages contain game-derived code and data, unlike the source checkout.

Threaded WebAssembly needs cross-origin isolation. `scripts/serve.py` supplies the required headers and supports HTTP Range requests. For an HTTPS static host that supports custom headers, configure:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Resource-Policy: same-origin
```

The package includes `coi-serviceworker` for hosts without custom headers. Keep all runtime companions, notices, and licenses together when moving the package.

The [Pages workflow](.github/workflows/pages.yml) builds documentation, downloads the release pinned in [PAGES_PLAYER.json](docs/PAGES_PLAYER.json), verifies its archive and companion files, and installs the player at the site root. Updating source alone keeps the pinned player. The [static hosting guide](docs/STATIC_HOSTING.md) explains the shared packaging and isolation design; its NCAA commands and URLs refer to the earlier NCAA port. Use the Madden commands above for this fork.

## How it works

1. **Static recompilation:** PSPRecomp analyzes the decrypted Allegrex/MIPS executable and emits C++. EA audio-bank programs are also translated ahead of time.
2. **WebAssembly:** Emscripten compiles the generated code and host runtime into the browser build.
3. **PSP services:** The host implements threads, synchronization, memory, file I/O, controller input, audio, savedata, and utility dialogs.
4. **Graphics and audio:** A GE display-list decoder renders through WebGL2, with a graphics worker where supported. PCM and decoded audio reach the browser through an AudioWorklet.
5. **Disc access:** The player reads ISO file and sector ranges locally. Development builds can instead stream the extracted disc over HTTP.

The game executable runs as translated code, without a PSP CPU interpreter or JIT. Compatibility still depends on the runtime's PSP service and graphics implementations. Movie playback and other hardware behavior remain incomplete.

### Repository layout

| Path | Contents |
| --- | --- |
| `profile/host/` | PSP services, renderer, audio, and native frontends |
| `profile/web/` | Browser shell, local ISO reader, launcher, and static bootstrap |
| `profile/tests/` | Host runtime regression tests |
| `profile/translator/` | Browser executable decryption and translation tools |
| `scripts/` | Setup, generation, build, packaging, serving, and checks |
| `patches/` | Patches applied to the pinned PSPRecomp checkout |
| `docs/` | Technical reports, reference comparisons, and hosting notes |
| `PSPRecomp/` | Fetched compiler framework; ignored |
| `games/madden10/` | Local disc, decrypted executable, manifest, and preload; ignored |
| `profile/generated/madden10*` | Generated game and audio translations; ignored |
| `build/` | Local binaries, packages, diagnostics, and test outputs; ignored |

## Documentation

- [Audio and frame pacing audit](docs/AUDIO_STUTTER_AUDIT.md): Madden boot evidence, recent fixes, and remaining stalls.
- [Runtime internals](docs/internals.md): architecture and debugging tools inherited from the base runtime.
- [Static hosting](docs/STATIC_HOSTING.md): shared deployment design and earlier NCAA examples.
- [NCAA Football 10 port](docs/NCAA10.md): separate title setup, gameplay evidence, and diagnostics.
- [NCAA remaining gaps](docs/REMAINING_GAPS.md): title-specific acceptance checks.
- [PPSSPP reference work](docs/PPSSPP_REFERENCE.md) and [GE replay performance](docs/GE_REPLAY_PERFORMANCE.md): renderer comparisons and measurement limits.

### Build NCAA from a fresh checkout

After the shared setup step, extract your NCAA Football 10 USA ISO to `games/ncaa10/root/disc` and place its decrypted executable at `games/ncaa10/root/EBOOT.BIN`. Then run:

```powershell
python scripts/verify_ncaa10.py games/ncaa10/root
./scripts/build_ncaa_web.ps1 -Iso "C:/path/to/NCAA Football 10.iso" -Emsdk ./tools/emsdk -Serve
```

Open **http://127.0.0.1:8621/**. This is the separate `ULUS10440` target; Madden and NCAA executables, generated sources, and disc files are not interchangeable. See [NCAA10.md](docs/NCAA10.md) for its controls and known limitations.

## Checks and contributions

For documentation and Pages changes:

```powershell
python scripts/test_pages_site.py
python scripts/test_pages_player.py
```

For browser runtime changes, relevant source-level checks include:

```powershell
node scripts/test_local_iso.mjs
node scripts/test_web_input.mjs
node scripts/test_web_audio.mjs
node scripts/test_web_saves.mjs
node scripts/test_web_persistence.mjs
python scripts/test_static_package.py
```

Synthetic checks do not establish game compatibility. When reporting a game issue, include the disc ID, source revision or release, browser, hardware, reproduction steps, and any `?profile` observations. Keep ISO files, decrypted executables, generated game code, memory dumps, and personal saves out of source contributions.

## Credits and license

Based on [PSP Web Recomp](https://github.com/snuri00/psp-web-recomp) by Samir Nuri, using [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp), Emscripten, and PSP hardware research from PPSSPP and JPCSP. The ATRAC decoder comes from PPSSPP's standalone FFmpeg sources; static isolation uses `coi-serviceworker`.

Project code is available under the [MIT License](LICENSE). The ATRAC decoder retains its [LGPL 2.1-or-later license](profile/third_party/at3_standalone/COPYING.LGPLv2.1). See [third-party notices](THIRD_PARTY_NOTICES.md) for attribution and component details.

No game disc, decrypted executable, generated game translation, or compiled game runtime is included in the source repository. Use a disc image you own and have the right to use; game-derived outputs retain the rights of their respective owners. Madden NFL, NFL, EA, PSP, and other names belong to their respective owners. This project is not affiliated with or endorsed by EA, the NFL, Sony, or their partners.
