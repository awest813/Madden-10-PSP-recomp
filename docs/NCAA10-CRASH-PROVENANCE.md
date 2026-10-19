# NCAA Football 10: null destination provenance

## Result

A fresh browser run of Fresno State at San Diego State, Qualcomm Stadium,
DAY, connects the failed animation allocation to the exact fault through
one host provenance ID (`1`) and guest descriptor tag (`0x0140065C`).
The allocator returns zero; the downstream stores and reader preserve it.
This run establishes insufficient contiguous heap space, rather than a
pointer lost between the allocator and reader. Why the cold load produces
this fragmented layout remains unresolved; no workaround or gameplay fix
for this failure is claimed.

The input is NCAA Football 10 PSP USA, `ULUS10440`. EBOOT.BIN SHA-256 is
`2B743DC6D746D822969A9B967B13422A369FF03C6A3A3E2604E051C1642B94F6`.
The browser executes its statically generated C++/WebAssembly corpus.

Evidence, relative to the project root:

- `build/ncaa-provenance-day-browser.log`: unmodified browser log text.
- `build/ncaa-provenance-day-events.log`: selected provenance, heap and fault events.
- `build/ncaa-provenance-day-options.jpg`: matchup, stadium and DAY selection.
- `build/ncaa-provenance-day-crash.jpg`: the halted browser session.
- `build/ncaa-provenance-allocator-disassembly.txt`: bounded ELF disassembly.
- `build/ncaa-disc-reader-disassembly.txt`: the surrounding reader function.

## Five checkpoints from the same browser run

| Checkpoint | Observed evidence |
| --- | --- |
| Allocator return | At `08CDB4A4`, request `39480`, `v0=0`, owner `092E6170`; allocator call at `08CDB49C`. Vcount 7838, thread 260 `user_main`. |
| Request descriptor | `092CD630`, tag `0140065C`; destination field `092CD650=0`, last observed writer `08C79518`. File stream `092D76D0`, offset `00A6BA80`, count `20000`. |
| Reader entry | At `08C7ECFC`, destination `0`, same descriptor/tag; thread 283 `DVDUMD_READ`, vcount 7852. Submission at `08C7F660` uses fd `12B`, vcount 7851. |
| Reader stack slot | `09FBB244=0`, written by `08C7ED04`, loaded by `08C7EDAC` into `s1` in a taken branch-likely delay slot. |
| Final store | `08C7EE50`, word `ACA40000`, `sw a0,0(a1)`, `a0=FD4AC67A`, `a1=0`, effective address `0`, width 4, `FAULT-no-write`. |

All addresses and sizes in this report are hexadecimal unless otherwise
stated. Thread IDs and vcounts are decimal.

The observed writer chain is:

| PC | Instruction | Destination address | Written value |
| --- | --- | --- | --- |
| `08CDB4A4` | `sw v0,28(fp)` | `092E618C` (owner buffer) | `0` |
| `08CCC644` | `sw s6,4(s0)` | `092D9554` (stream request buffer) | `0` |
| `08C7462C` | `sw s1,20(s4)` | `092D76E4` (file stream base) | `0` |
| `08C74640` | `sw s1,44(s4)` | `092D76FC` (file stream current) | `0` |
| `08C79518` | `sw s1,32(s3)` | `092CD650` (descriptor destination) | `0` |
| `08C7ED04` | `sw a1,36(sp)` | `09FBB244` (reader destination slot) | `0` |

Writes are logged as completed only after the following instrumented
instruction confirms the value in the same execution context. The current
observer also requires that instruction to be the adjacent PC. These are
last **observed** writers in the selected units, not a global memory-write
history. The final invalid store is rejected before access and has no
completed-write event.

## Allocation failure

At the failed request, heap `08EC9020` has three free nodes:

| Node | Block bytes including header | Usable bytes |
| --- | --- | --- |
| `09DC27F0` | `E80` | `E70` |
| `09E1FFA0` | `28950` | `28940` |
| `09EAC1B0` | `391C0` | `391B0` |

Total usable free space is `62960`; the largest usable block is `391B0`,
which is `2D0` (720 decimal) bytes smaller than the `39480` request.
The free-list walk terminates at the expected sentinel, `08EC9030`.
The game's capacity query returns the largest block including its 16-byte
header, `391C0`, which is also below the request.

The browser log observes the capacity result and allocation return.
Disassembly identifies the rejecting path:

```text
08CD3080 0051202B  sltu a0,v0,s1  # 391C0 < 39480
08CD3084 14800007  bne a0,zero,08CD30A4
08CD3088 00000000  nop            # delay slot
...
08CD30C4 00001025  or v0,zero,zero
08CD30C8 8FBF0200  lw ra,512(sp)
08CD30CC 03E00008  jr ra
08CD30D0 27BD0210  addiu sp,sp,528 # delay slot
```

The low-level allocation call is bypassed when this capacity check fails.
A synthetic fixture executes the actual generated unit from `08CD3080`
with the captured capacity/request and confirms a zero return through this
path. The intermediate PCs above are static control-flow evidence, not
additional instruction events captured in the browser.

The runtime's corrected 4096-byte LowAligned reservation is already present:
`UserSbrk 08EC9000 + 01036400`. Its initial reservation was compared with an
official PPSSPP reference. This alignment correction does not eliminate the
DAY failure. The EA heap's raw header includes `08EC9010` and `09EFD3E0`;
the complete header and free-node words remain in the raw log.

The `79C0` loading-bank buffer is at `09E48900`. Its free-entry observation
at vcount 7838 follows the failed animation return. That records release
ordering, not proof that the release completed or should occur sooner.
The next runtime comparison should examine allocation order and asset
lifetimes against the same cold DAY start on a PSP reference.

## Reader instruction sequence

The raw trace preserves executed delay slots. The decisive segment is:

| Executed PC | Decoded operation | Observed effect |
| --- | --- | --- |
| `08C7EDA4` | `andi a0,v0,0x20` | `a0: 0 -> 20`, with `v0=32` from the preceding UMD-status call |
| `08C7EDA8` | `bnel a0,zero,08C7EE2C` | Taken |
| `08C7EDAC` | `lw s1,36(sp)` | Delay slot; reads `09FBB244=0`, `s1: A6BA80 -> 0` |
| `08C7EE2C` | `lui a0,08E3` | `a0: 20 -> 08E30000` |
| `08C7EE30` | `lbu a0,0x1128(a0)` | `a0: 08E30000 -> 0` |
| `08C7EE34` | `lw a2,44(sp)` | `a2: A6BA80 -> 0` |
| `08C7EE38` | `andi a0,a0,0xFF` | No GPR change |
| `08C7EE3C` | `bne a2,zero,08C7EECC` | Not taken |
| `08C7EE40` | `or a1,s1,zero` | Delay slot; `a1: 08EC5818 -> 0` |
| `08C7EE44` | `bnel a0,zero,08C7EE90` | Not taken; `08C7EE48` is annulled |
| `08C7EE4C` | `lw a0,0x112C(fp)` | `a0: 0 -> FD4AC67A` |
| `08C7EE50` | `sw a0,0(a1)` | Address `0`, width 4; FAULT, no write |

The raw browser artifact predates two diagnostic wording corrections:
`after-changes` means changes up to the next observed PC, potentially
including an uninstrumented call. In particular, the `v0` change listed
after the NOP at `08C7EDA0` belongs to the uninstrumented UMD-status call.
It is not an effect of the NOP. The current renderer labels these deltas
`changes-at-next-observed-pc` and states this limitation explicitly.

Also, the old `file-stream-return ... construction-link-matches=0` line
compares a returned handle (`7837`) to an object address (`092D76D0`). The
raw stream snapshot contains `7837` in its first word. The observer now
checks the handle against that word and labels the handle/object separately.
This line is not evidence of pointer loss.

## Validation and reproduction

### MAIN heap audit and reference comparison

The first MAIN capacity query (`size=294`) was captured in this port and
official PPSSPP v1.20.4, PSP-1000 configuration, from the same local ISO.
The MAIN header and all five initial physical nodes match byte for byte:
heap `08EC9020`, first node `08EC9010`, end sentinel `09EFD3E0`, and initial
free span `C416D0`. Both use `UserSbrk=08EC9000`, size `01036400`.
The physical node interval is `010343D0` bytes; the partition has another
`2030` bytes outside that interval (a `10` prefix and `2020` tail). Their
purpose is not inferred merely from those lengths.

At this checkpoint, MAIN contains DB and system subheap reservations of
`274400` and `17E800` payload bytes, 32 bytes of allocation headers, a
`E0`-byte control-node span, and the `C416D0` free span. This accounts for
the whole physical interval. These parent reservations must not be counted
again as additional MAIN allocations when walking their child heaps.

The first **377 MAIN wrapper allocation events** also match in requested
size, returned address, alignment, flags, continuation and caller. Event 378
differs: PPSSPP allocates `E0` bytes through `08CBC7B4`, whereas the port
allocates `10` bytes through `0887782C`. `08CBC7B4` returns from the allocation
callback `08CBC7A0`, beside the movie code whose public entry `08CBC838`
the port deliberately skips. This startup difference has not been shown
to cause the later DAY failure. A comparison of the same cold DAY load
through PPSSPP is still incomplete: its debugger framebuffer downloads
returned `Could not download output`, including the attempted software
rendering run. No visually verified reference matchup or gameplay is claimed.

A separate, manually verified native cold **Fresno State–San Diego State,
Qualcomm DAY** run reproduces the same `39480` request / `391C0` capacity
rejection at vcount 1793. This run selected the `9340` loading sound bank;
the earlier browser run selected `79C0`. The largest free region is identical
in both. In this native capture:

| MAIN accounting | Bytes |
| --- | ---: |
| Requested payload in 424 allocated nodes | 16,558,638 |
| Allocated-node headers and absorbed/alignment padding | 34,994 |
| Control-node physical span | 224 |
| Three free spans, including their headers | 397,328 |
| Complete physical interval | 16,991,184 |

The three free spans start at `09DC27F0` (`E80`), `09E1FFA0` (`26FD0`),
and `09EAC1B0` (`391C0`). Usable total is `60FE0` = **397,280 bytes**,
which exceeds the request. A full physical-chain walk validates forward and
back links, span lengths, free-list membership and free-list links. There
are **no adjacent free nodes** to merge at rejection. This rules out an
unmerged adjacent pair in this captured MAIN heap, not every possible
allocator defect in every scenario.

The 704-byte difference is the capacity query's raw-span comparison.
For the largest block, payload begins no earlier than `09EAC1C0` after its
16-byte header. The requested 128-byte alignment moves that to `09EAC200`,
leaving `39170` usable bytes before `09EE5370`: the aligned payload is
**784 bytes** short (`704 + 16 + 64`). There is no unexplained 704-byte hole
in the accounting and no evidence that adding exactly 704 bytes fixes this
layout.

Concrete live separators and lifetimes are now recorded:

* `09DF1A00`, size `2E598`, caller `08CDB4A4`: the preceding animation
  input allocation. Its free entry is later, at vcount 1797.
* `09E46F80`, size `9340`, caller `08B31A9C`: the loading bank, between
  the second free region and the next allocation.
* `09E50300`, size `5BEB0`, caller `08CC4728`: `SDSTAT_Fair.viv|SDSTAT_Fair.ord`,
  immediately before the largest free region.
* `09EE5380`, size `1880`, caller `08CFBA50`: the live allocation immediately
  after that region. Its free entry through `08CFBB70` occurs after the
  failed animation return, along with 17 other releases through that caller,
  in the same vcount 1793. These are free-entry observations; they do not
  by themselves prove completion or justify moving the releases earlier.

This narrows the next comparison to asset lifetime/release ordering around
`08CFBB70`, and the animation loader's overlapping input allocations.
The cold DAY crash is **not fixed** by these diagnostics.

Evidence in `build/`:

* `ncaa-heap-initial-comparison.json`, `ncaa-heap-allocation-prefix-comparison.json`.
* `ncaa-main-heap-initial-audit.json`, `ncaa-main-heap-day-failure-audit.json`.
* `ncaa-heap-interactive-day-{init,target}.ram`, `ncaa-heap-replay.log`.
* `ncaa-heap-interactive-day-options.png`, `ncaa-heap-interactive-day-matchup.png`.
* `reference-ppsspp/pool-first-main-allocation.ram`,
  `reference-ppsspp/heap-comparison.log` (allocation prefix).

`scripts/analyze_ncaa_heap.py` reads a 32 or 64 MiB RAM dump beginning at `08000000`.
For example:

```powershell
python scripts/analyze_ncaa_heap.py build/ncaa-heap-interactive-day-target.ram --ledger build/ncaa-heap-replay.log --out build/ncaa-main-heap-day-failure-audit.json
```

The opt-in MAIN **wrapper** ledger requires instruction units `0287;0307`,
`PSPWEB_TRACE_NCAA_IO=1` and `PSPWEB_TRACE_NCAA_HEAP=1`. It records allocations
at `08CD3224` and low-level free entries at `08C8225C`; it is not a complete
history of direct low-level allocations or reallocations. Caller stack slots
are interpreted only for the recognized wrappers. Set
`PSPWEB_NCAA_HEAP_CAPTURE=<local-prefix>` for read-only native snapshots at
the first MAIN capacity query, first `39480` query, and following MAIN query.
The last capture is a boundary observation; do not assume it follows a
particular release without checking the ledger.

Native debugging also supports `--control-file <path> --realtime 1`.
Each new line `serial buttons_hex lx ly frames run|pause|quit` queues a PSP
controller pulse, releases it after the specified host steps, or pauses/quits
the host. Pausing writes the configured `--dump` framebuffer. Input serials
and host frames are logged for replay. This does not write guest memory.

### Gameplay regression

The normal browser build reached Kentucky away at Florida State home,
Doak Campbell Stadium DAY. Manual kickoff return reached the 13-yard line.
After a delay penalty, a manually selected Ace-Big / 16 Power O gained four
yards to 2nd-and-11 at the 12, 4:46 remaining. The live handoff capture is
`build/ncaa-provenance-uk-fsu-live-gameplay.jpg`; matchup and browser log are
saved beside it. This is a gameplay smoke test, not a fix for the separate
cold Qualcomm DAY failure.

### Diagnostic fixtures

The synthetic fixture uses real generated code for capacity rejection,
buffer propagation, descriptor construction, reader prologue and both
faulting prefix orders. External object allocations and the public I/O
submission frame are synthetic; it performs no disc read. It also compares
full guest context and stack bytes with the reader logger enabled/disabled.
Full provenance tests passed with the selected units at O0 and O1.
The framework suite and all ten native host checks pass.

To enable the five-unit diagnostic build:

```powershell
cmake -S PSPRecomp -B build/web-ncaa10 '-DPSPWEB_INSTRUCTION_TRACE_UNITS=0284;0285;0286;0306;0309'
./scripts/build_ncaa_web.ps1 -Iso NACC_Football_10_USA_PSP-PSPKiNG.iso
# Open http://127.0.0.1:8621/?profile&disctrace after serving the build.
```

Use the same unit list in `build/native-ncaa10`, build
`pspweb_disc_reader_tests`, then run CTest from
`build/native-ncaa10/profiles/web`. CTest enables full provenance assertions
only when all five units are traced; unit 0286 alone enables reader fault
assertions. Clear `PSPWEB_INSTRUCTION_TRACE_UNITS` and
`PSPWEB_INSTRUCTION_TRACE_OPT_LEVEL` for normal production builds.
The current production binaries have those diagnostics compiled out.

No guest zero-page mapping, replacement buffer, early bank free, skipped
store or EA allocator patch is part of this investigation.

### Larger heaps (2026-10-12)

At the user's request, NCAA now defaults to 64 MiB guest RAM in both native
and browser builds. Open `http://127.0.0.1:8621/` without a RAM flag. Use
`PSPWEB_RAM_MB=32` (native) or `?ram=32` (browser) for the original layout;
explicit `64` is also accepted. Other values are rejected at startup, with
a visible browser error. This
constructs a real 64 MiB guest RAM buffer and exposes a 56 MiB user partition,
ending at `0x0C000000`. The compatibility mode uses 32 MiB RAM / 24 MiB user
partition. Other titles and the core runtime retain their existing defaults.
Thread stacks, partition free-size queries and allocation placement use the
selected limit. Framebuffer validation, relocated AOT audio scanning and RAM
diagnostic captures also use the selected backing size.

The game sizes its own pools from SysMem. In the extended run it requested
`UserSbrk 08EC9000 + 01F40000`, and MAIN ended at `0AE06FE0`. No EA heap header,
allocation return or guest instruction is patched to obtain this growth.

The native test cold-booted with the same empty diagnostic memory stick,
selected Fresno State away / San Diego State home, and visually verified
Qualcomm Stadium DAY before loading. The failing allocation checkpoint now
has this comparison:

| Measurement | Original 32 MiB | Default 64 MiB |
| --- | ---: | ---: |
| MAIN physical interval | 16,991,184 B | 32,759,760 B |
| Live allocated nodes | 424 | 424 |
| Requested live payload | 16,558,638 B | 16,558,638 B |
| Allocated headers and padding | 34,994 B | 35,010 B |
| Total usable free bytes | 397,280 B | 16,165,840 B |
| Largest free span | `391C0` | `ED4CD0` |
| `39480` allocation result | null | `09E7BF00` |
| Heap integrity errors | none | none |

The 64 MiB match reached the coin toss, play selection, kickoff and return.
The 32 MiB cold-load fragmentation/order problem remains; the larger-memory
option is a tested mitigation rather than an explanation or repair of that
original failure. The explicit 32 MiB startup's MAIN header and all five
initial physical node headers still match the previous 32 MiB capture exactly.

Both native and web production builds passed. All ten host tests passed,
including 32/64 MiB partition capacity, high allocations, exhaustion/release,
upper-RAM AOT cached/uncached access and display address limits. Invalid
`PSPWEB_RAM_MB=48` was rejected with exit status 1. A fresh browser session
with `?ram=64&profile` also reached Kentucky away / FSU home at Doak Campbell
Stadium DAY. Manual kickoff and defensive inputs worked; the match progressed
to 1st-and-10 at the 48 with about 30 FPS / 100% speed. The saved gameplay
frame has a penalty flag following defensive movement. Audio had six underruns
by that capture; this is a short gameplay smoke test, not a complete-game test.

Evidence:

- `build/ncaa-ram64-day.log`, `ncaa-ram64-day-init.ram`,
  `ncaa-ram64-day-target.ram`, `ncaa-ram64-initial-audit.json`,
  `ncaa-ram64-day-target-audit.json`, `ncaa-ram64-heap-comparison.json`.
- `build/ncaa-ram64-day-options.png`, `ncaa-ram64-day-matchup.png`,
  `ncaa-ram64-day-kickoff.png`, `ncaa-ram64-day-gameplay.png`.
- `build/ncaa-ram64-uk-fsu-matchup.jpg`,
  `ncaa-ram64-uk-fsu-day-options.jpg`,
  `ncaa-ram64-uk-fsu-live-gameplay.jpg`,
  `ncaa-ram64-uk-fsu-browser.log`.
- `build/ncaa-ram64-native-build.log`, `ncaa-ram64-web-build.log`,
  `ncaa-ram64-host-tests.log`, `ncaa-ram32-regression.log`,
  `ncaa-ram-invalid-option.log`.

### Default-mode audit (2026-10-12)

The native startup matrix passed with the RAM setting unset, explicitly `32`,
and explicitly `64`; invalid `48` and `bad` values exited with status 1.
Unset and explicit `64` produced byte-identical MAIN headers and all five
initial physical node headers. Explicit `32` also retained byte-identical
headers against the earlier 32 MiB regression capture. Both layouts passed
the heap integrity audit. These comparisons cover heap initialization, not
every byte of RAM or subsequent allocation order.

Framebuffer setup now requires the actual RAM size as an explicit argument.
The memory regression exercises the public `sceDisplaySetFrameBuf` import,
including the upper-memory alias and rejection at the selected RAM limit.
Cached AOT byte, halfword and word access is also checked at the RAM boundary.
All ten host suites pass; native and web production builds completed with
instruction tracing disabled. An invalid browser override was checked
visually and displays `Stopped: RAM must be 32 or 64 MiB`.
The browser audio, on-screen keyboard and controller regression scripts also
pass (`build/ncaa-default64-web-{audio,osk,input}-tests.log`).

Audit evidence: `build/ncaa-default64-native-matrix.json`,
`ncaa-default64-initial-layout-comparison.json`,
`ncaa-default64-default-heap-audit.json`,
`ncaa-default64-invalid-browser.jpg`, `ncaa-default64-host-tests.log`,
`ncaa-default64-native-build.log`, and `ncaa-default64-web-build.log`.

A fresh browser boot at `http://127.0.0.1:8621/?profile` (no RAM override)
reported 64 MiB / 56 MiB and loaded Kentucky away / Florida State home,
Doak Campbell Stadium DAY. After receiving kickoff at the 33 and two
inspection-delay penalties, a manually selected Ace-Big / 16 Power O
accepted a snap and analog input, ending 2nd-and-20 at the 23. The live
handoff capture reports 30.1 FPS / 101% speed. The play clock was temporarily
disabled for that input check and visually restored to On afterward; the
match is left paused. The session accumulated 15 audio underruns, so this
check does not establish uninterrupted audio, sustained pacing or reference
rendering accuracy.

Browser evidence: `build/ncaa-default64-uk-fsu-matchup.jpg`,
`ncaa-default64-uk-fsu-day-options.jpg`,
`ncaa-default64-uk-fsu-live-gameplay.jpg`,
`ncaa-default64-uk-fsu-run-result.jpg`,
`ncaa-default64-play-clock-restored.jpg`, and
`ncaa-default64-uk-fsu-browser.log`.
