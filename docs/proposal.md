# Amiga File System Test Harness — Proposal

Oct 21, 2026 · @Simon Dick

A single test harness, working name **fstest**, that shows, for a stated set of packets, OS versions and declared deviations, that an Amiga file system handler behaves as the AmigaDOS Manual says, that file contents survive the operation sequences it ran, and that the volume survives the crashes, lying drives and bit rot it injected. It is format-agnostic, so it tests Phoenix, FFS, PFS3 and SFS alike and the host crates against each other.

## Purpose and scope

**In scope**

- Handler-level conformance: the packet protocol, error codes, edge cases and version behaviours documented in the AmigaDOS Manual.
- File-level correctness: random operation sequences checked against a model, and churn under sustained load.
- Crash and fault survival: power cut, reset mid-commit, memory corruption, misbehaving devices, media removal, in Copperline.
- Cross-implementation agreement: the Amiga handler against the host implementation of the same format.
- Reproducibility: every run seeded and logged so a failure on hardware replays in an emulator.

**Out of scope: devsoak's job.** devsoak is a separate binary that tests block device *drivers* (trackdisk-style commands under heavy traffic, driver quirks). fstest sits above it and assumes the device works; the only shared piece is the Copperline device-fault shim, which both consume. Neither depends on the other at build time, both ship under BSD 2-Clause as devsoak does, and a driver bug found by fstest is handed to devsoak to reproduce at its own level.

**Also out of scope:** performance benchmarking (DiskSpeed remains the published number), repair tools, and host-side fuzzing of a format's parser (that lives with each format's own crate).

## What a pass means

Every result carries a verdict, the oracle that produced it and the coverage unit it applies to, so a green run says exactly what was shown and nothing more.

**Verdicts:** `pass`; `fail` (expectation not met); `deviation` (handler behaves differently from the manual, and a quirks entry declares and explains it; reported, never hidden); `unsupported` (the tier, OS version or handler cannot run the test; always listed, never silently skipped); `inconclusive` (crash, hang or harness fault; image preserved, run aborted).

**Coverage unit for layer 1:** packet × DOS version × outcome (success or each documented error) × handler state (fresh mount, after inhibit/uninhibit, after a failed operation, with the object locked by another process, on a write-protected volume). A layer 1 report states which cells were exercised; "every DOS packet" is the target matrix, not a claim.

**Oracle classes:** `independent` (the AmigaDOS Manual, or a reference handler run under amiga-fuse/volamos); `model` (the harness's in-memory expectation, portable across handlers); `correlated` (the host implementation of the same format, which shares the C core with the Phoenix handler and so can only catch environment-dependent bugs such as codegen, alignment and driver effects, not shared logic errors). Every verdict names its class.

**Durability boundary for crash tests:** data is *acknowledged* only when `ACTION_FLUSH` has returned, or, for Phoenix, when a commit has been reported through `GET_STATS` or `ACTION_FLUSH`. A successful `Write()` alone acknowledges nothing. After recovery, acknowledged data must be intact; unacknowledged data may be present or absent but never corrupt; metadata must be consistent in both cases. Each target format and tier states what `ACTION_FLUSH` promises there (FFS ≤ V40 replied before the write finished, which the quirks file records).

**What a pass does not establish:** correctness under hardware timing the campaign did not run, correctness of the block device driver (devsoak's job), resistance to faults the campaign did not inject, or soundness of the on-disk format when the only oracle was correlated.

## Architecture

&#91;embedded content: fstest architecture · 4 layers, 3 oracles, 3 tiers\]

One Amiga test binary runs the four layers against any mounted volume; each layer hands its verdict to an oracle rather than trusting the handler under test, and the same binary with the same seeds runs on all three execution tiers so a hardware failure replays in an emulator.

## Layer 1: packet conformance

A table-driven suite that sends every documented DOS packet by hand (`DoPkt()`), with the AmigaDOS Manual as the oracle for arguments, results, `dp_Res2` error codes and the edge cases it calls out. Nothing like it exists today, and it is useful to every handler author, not only to Phoenix.

**Coverage**

- All file, lock, examine, link, metadata, notification, volume and control packets in the manual's packet chapter, each with its success case, each documented error case, and the BPTR/BSTR argument conventions.
- Documented quirks checked explicitly: `ACTION_MORE_CACHE` returning the count in `dp_Res1` as `AddBuffers` expects; `ACTION_FLUSH` not replying before data is on the medium; `ACTION_SEEK` past 2 GB following the suggested 64-bit convention; `ExAll()` with `ED_OWNER` and the matcher hook; soft-link resolution via `ERROR_IS_SOFT_LINK`; exclusive lock on the root refused; `ACTION_SET_FILE_SIZE` clamping other file handles' pointers.
- dos.library-level checks of the same behaviour (`Open`, `Lock`, `Examine`, `SetFileSize`, `MakeLink`...), since the library wrappers add their own conventions.
- Version awareness: each test carries the manual's version annotation and is skipped or expected to fail with `ERROR_ACTION_NOT_KNOWN` on handlers that predate it.

**Safe probing.** Handlers that switch on `dp_Type` without a default, or dereference `dp_Arg1` before checking the type, crash on packets they do not know. The suite therefore:

1. Establishes a baseline first: `ACTION_IS_FILESYSTEM`, `ACTION_NIL`, `ACTION_CURRENT_VOLUME`, a lock on the root and its release.
2. Sends packets the handler may not implement only in an opt-in *probe* mode, from that baseline state, with every unused argument zeroed, and logs the packet number before sending so a crash is attributable.
3. Never sends third-party or custom packets (PFS3, SFS, Phoenix private ranges) unless the handler's identity has been confirmed via its own capability query.
4. Identifies the handler before anything else: DosType from the DosList entry, the handler's version string from its segment list where present, and \`QUERY\_CAPS\` / \`ACTION\_FILESYSTEM\_ATTR\` / \`ACTION\_QUERY\_ATTR\` where answered; if identity cannot be established the suite runs only the baseline and reports the rest as unsupported. A crash or hang (watchdog timeout from a second process) marks the test inconclusive, preserves the volume image untouched, and aborts the run rather than continuing on a possibly damaged handler. Keeps a per-handler quirks file in the same format and with the same semantics as devsoak.quirks: match conditions (handler name, version range, Kickstart version), actions (skip a packet, downgrade a test, expect a deviation), confidence tiers where only \`confirmed\` entries apply automatically and \`reported\` or \`suspected\` ones need \`-k ID\`, and \`-K\` to ignore the file so a crash counts as a finding.

**Output:** one line per test with verdict, oracle class, the manual section it comes from, and every quirk that was applied (its id, confidence and the match reason), machine-readable for CI and readable in a Shell. Applied quirks are listed at the top of the report, not buried, so a quirk can never silently hide a regression.

## Layer 2: random operations against a model

An fsx-style tester: a seeded generator picks operations, applies each to the real volume and to an in-memory model, and compares after every step. The first divergence is the bug, and the seed reproduces it anywhere.

- **Operations:** create, open in each mode, read, write, seek (all three modes, including past EOF and beyond 2 GB where the volume allows), `SetFileSize` grow and truncate, close, delete, rename within and across directories, `CreateDir`, hard and soft links, protection bits, comments, dates, `ExAll` and `ExNext` walks, record locks, `Inhibit` on and off, and `Flush`.
- **Model:** A tree of names to byte arrays with the metadata the API exposes, split into \*portable\* expectations (contents, sizes, existence, fold-insensitive name identity, protection bits, basic error codes) and \*handler-specific\* ones (ExAll ordering, date granularity, soft-link resolution, error precedence when several apply, lock-ownership rules), the latter loaded per handler from the quirks file. The generator issues invalid operations as well as valid ones (open a directory as a file, delete a locked object, rename across a non-existent path) so error paths are modelled too. Model and log are bounded and measured: the tool reports its working set at start, refuses to run a configuration it cannot fit, and runs reduced profiles on small machines; 4 MB is the comfortable target, not an assumption. Content is generated from the seed and offset rather than stored, so large files cost nothing in the model.
- **Checks after each step:** result code and `IoErr()` against the model's expectation; file contents by reading back; directory listing equality; `fib_Size` and dates. Every N steps a full-tree compare.
- **Shrinking:** on a failure the tester minimises the sequence by delta debugging (removing subsets, not only truncating), keeping dependent pairs together (open/close, lock/unlock, create/delete of the same object) so the result is still a valid program, then prints it as a script that the conformance layer can run as a regression test.
- **Knobs:** operation mix, file-size distribution (biased to inline, extent-boundary and multi-extent sizes), directory fan-out (to push B-tree splits and FFS hash chains), name lengths up to the limit and names that differ only in case.
- **Concurrency mode:** two or three tester processes on the same volume with disjoint subtrees, since handler reentrancy around the packet loop is where many bugs live. Shared files with record locks are a separate mode.
- **Packet mode:** the same generator can send each operation as a raw packet via `DoPkt()` instead of through dos.library, so the model compare also covers what the library wrappers normally mask (argument normalisation, BSTR construction, path splitting, error-code translation). Runs in the two modes over the same seed are compared against a declared list of permitted differences (dos.library normalises paths, translates some error codes and retries some operations); a divergence outside that list is a finding about the wrapper or the handler's raw-packet handling, and the list itself is maintained from the manual.

## Layer 3: churn and soak

Hours of mixed load to find what only time and fragmentation expose: allocator leaks, bitmap drift, directory structures that degrade, cache eviction bugs, and the slow corruption that a bad `MaxTransfer` or a marginal cable produces.

- **Workload:** the FileSystemsStressTest pattern generalised: seeded random files, hash on write and on read-back, delete a percentage and regenerate at the same total size, repeat; plus directory-heavy phases (thousands of entries, then delete every other one), large sequential files, and a long-lived set of files rewritten in place to exercise `nocow`-style paths.
- **Invariants checked between phases:** free space within a format-specific bound of the live file total (directories, metadata, allocation units and reserved blocks make an exact match impossible; the bound is declared per format in the quirks file); `Info()` self-consistency; a full `ExAll` walk matching the expected set; for formats with a host checker, an unmount and host-side verify at the end of each hour as a separate check.
- **Fragmentation report:** extent count per file and free-extent histogram where the handler exposes them (Phoenix `GET_STATS`), otherwise sequential-read rates before and after are recorded as a performance signal only, since they are too noisy to call a fragmentation measurement.
- **Duration tiers:** 10 minutes for nightly emulator runs, 8 hours for hardware soak before a release.
- **Resumable:** the run state (seed, phase, iteration) is written to a separate volume so a crash mid-soak is itself a reportable event with a known position, not a lost night. If that volume is unavailable or its log fails its own checksum, the run restarts from the seed and reports the loss of position rather than guessing.

## Layer 4: fault injection in Copperline

The real handler, OS and driver under scripted breakage, driven from the host so every failure is reproducible to the cycle. This is the layer no existing Amiga tool provides and the one that matters most for a crash-safe design.

| Fault | Injection | Expected after recovery |
| --- | --- | --- |
| Power cut | Pause at a random instant during layer 2/3 load, snapshot the image, cold boot from the snapshot | Mounts without a validator; every file the handler acknowledged (flush or commit reported) is intact; nothing else is corrupt |
| Reset mid-commit | Breakpoint on the handler's barrier call, then reset between the data writes and the root flip | Previous root wins; the partial generation is invisible |
| RAM scribble | Pause, overwrite N bytes in the handler's cache, dirty buffers or lock list, resume | Checksummed buffers refused at commit; handler reports and continues or drops to read-only; nothing wrong reaches the disk |
| Lying drive | Device shim drops, reorders or tears sector writes, or acknowledges flush without honouring it | Safe mode's read-back verify catches it; volume stays consistent; fast mode is expected to fail and that is recorded |
| Bit rot | Flip bits in metadata and data blocks of the image between runs | Metadata checksum errors reported on access; data errors reported where data checksums are on; snapshots still serve the intact generation; scrub finds every flip |
| Media yank | Detach the host disk mid-write, reattach | Disk-change handled; nothing past the last commit believed; remount clean |
| Warm reset | Reset without clearing RAM, for handlers with a reset-surviving cache | Replay happens only when the area validates |

**What Copperline already provides.** Its control protocol (newline-delimited JSON-RPC 2.0 over TCP, with an MCP bridge via `copperline-ctl --mcp`) covers nearly all of this today: `pause`, `continue`, `step*` and `run_until`; `machine.reset` warm or cold; `mem.read`, `mem.write` and `mem.digest`; `break.add` (PC, watch, MMIO, catch, loadseg) and `last_writer`; `state.save`/`state.load` and `reverse_anchor` for power-cut replay; `copperhf.attach`/`copperhf.eject` and `pcmcia.insert`/`pcmcia.eject` for media yank; emulated-clock scheduling and `rtc.set` freeze for repeatability; `events.subscribe` for observing MMIO. devsoak already drives this path for eject and insert.

**The one proposal: a generic disk fault policy for Copperline.** Not copperhf-specific: it sits on the image-backed block layer behind any emulated device (copperhf unit, PCMCIA CF, IDE, SCSI, floppy), so the guest driver still sees real controller behaviour above it. Copperline's existing `fault.inject` is a memory bus-error injector and does not cover this. Specified jointly with devsoak.

- Methods: `disk.fault.set` (target, policy, seed, returns id), `disk.fault.clear`, `disk.fault.list`, `disk.fault.stats` (writes dropped, torn, reordered, flushes ignored, stale reads served: the proof a fault actually fired), and a `disk.fault` event family for `events.subscribe` so a fault correlates with the guest operation that hit it.
- Policy: an ordered rule list, each with a selector (`op` read/write/flush/trim/any, `lba` range, seeded `probability`, `after` N ops, `times`) and an action: `drop`, `tear` (at sector or byte N), `reorder` (window N, released by flush), `ignore` (flush acknowledged, held writes dropped or held), `stale` (serve pre-write data for N reads), `corrupt` (flip N bits), `error` (media, timeout, not\_ready, write\_protect), `delay` (emulated ms), `cut` (power loss at the device: in-flight and held writes lost, `not_ready` until cleared).
- Determinism: all randomness from the policy seed plus request sequence number; state snapshots include the held-write window and counters; physical host disks are refused as targets; nothing persists in config files.

**Verification:** always by the host implementation of the format reading the image after the fault, never by the possibly damaged handler. For formats without a host implementation (SFS), the check is a remount plus the format's own validator and a layer-2 model compare, which is weaker and is marked as such in the report.

**Statistics:** a fault campaign is a seed range, not a single run; thousands of power cuts at random points is what finds the one-in-a-thousand window.

**Staging.** The policy language is deliberately broad, but the campaign starts narrow: dropped writes, ignored flushes and power cuts first, with their recovery assertions validated against a known-good handler, then reorder, stale reads and torn writes once ordering semantics at the guest-visible block layer are pinned down (held writes are visible to reads, as a write cache would be; fault state survives `state.save`/`state.load`). Every event carries the target and request sequence number so a campaign can be audited after the fact.

**Format-dependent checks** (metadata checksums, data checksums, snapshots) are capabilities declared per format, not generic pass criteria; a format without them gets the `model`-class verdict for those rows rather than a fail, and the report level says so, so results across FFS, PFS3, SFS and Phoenix stay comparable.

## Oracles

A test is only as good as what decides "correct"; each layer names its oracle and the report says which one judged each result.

| Oracle | Judges | Strength | Available for |
| --- | --- | --- | --- |
| The AmigaDOS Manual, encoded as expectations | Layer 1 | Authoritative for protocol behaviour; where old autodocs disagree the manual wins and the test says so | Every handler |
| In-memory model | Layers 2 and 3 | Catches content and metadata divergence immediately; blind to on-disk structure | Every handler |
| Host implementation of the format | Layers 3 and 4, and end-of-run checks | Reads the bytes in a different environment. For Phoenix it shares the C core with the handler and is therefore *correlated*: it catches codegen, alignment and driver effects, not shared logic errors. For FFS and PFS3 the crates are separate code and count as independent once validated against the reference handlers | Phoenix (correlated); FFS and PFS3 (independent) via the Rust crates |
| Reference handler under amiga-fuse | Cross-checks of the host crates themselves | Shadow-compare of every operation against the original handler run under volamos | FFS, PFS3; Phoenix once its handler exists |

The reference-handler oracle closes the loop: the host crates are proven against the original handlers, then used to judge the new handler. Phoenix's own handler becomes a reference for its host implementation the same way, so the two can never drift silently.

## Execution tiers and reproducibility

The operation stream and seed are portable across tiers; each tier records a workload profile (model size, operation count, duration) and the capabilities it offers, and any test a tier cannot run is reported as unsupported rather than skipped. The tier decides how much runs and how often.

| Tier | When | Layers | Why |
| --- | --- | --- | --- |
| volamos on the host | Every commit, seconds | 1 and 2 against the handler run under volamos's handler hosting (or against the host crates directly) | Catches protocol and logic regressions before an emulator boots |
| Copperline and Amiberry | Nightly, minutes to an hour | 1 to 4, booted from RDB with real drivers (scsi.device, IDE, Copperline's own devices) | The independent witness: real OS, real drivers, fault injection |
| Hardware | Before a release, hours | 1 to 3 on a stock A1200 (floor) and an accelerated machine (full features) | Timing, DMA masks and controller quirks no emulator reproduces |

**Reproducibility rules**

- Every run is identified by harness version, handler name and version, volume geometry and `DosEnvec`, tier, and seed. A report line that lacks any of these is a bug in the harness.
- The tester logs each operation before performing it, to a separate volume or the serial port, so a crash leaves the last operation on record.
- A hardware failure is replayed in Copperline from the same seed first; if it does not reproduce there, the difference (timing, driver, DMA) is itself the finding and is recorded in the quirks file.
- Shrunk failing sequences from layer 2 are checked into the repository as named regression tests and run in layer 1's table from then on.
- Emulator configurations are generated from the harness's own TOML templates so a run is tied to a specific CPU, memory and device setup.

## Deliverables and relationships

**Amiga side (C, AmigaPorts GCC 16.2, `-m68020-60`, also built with 6.5 and vbcc in CI)**

- `fstest`: one CLI binary with subcommands `conform`, `random`, `churn` and `replay`, ReadArgs templates, exit codes for scripting, output to console, file or serial. Runs on 2.04 and up; version-gated tests skip themselves.
- `fstest.quirks`: the per-handler quirks file, shipped in the archive with known entries for FFS versions, PFS3-AIO, SFS and the RAM-Handler, and extended by users. It is the single source: the suite reads it to decide what to probe, and the build generates a documentation page from it (handler, version, packet, behaviour, workaround) so the published list can never drift from what the tool actually does. Contributions go in as entries to the file, and the page regenerates on release.
- A small helper library (packet sending, model, seeded generator) usable by other test tools.

**Host side (Rust, in the amiga tools repo)**

- `fstest-drive`: orchestrates tiers, generates emulator configs, injects faults through Copperline's scripting surface, runs the host-implementation verify, collects reports.
- The Copperline block-device fault shim, specified jointly with devsoak and implemented once.
- Report format and a summariser that turns a seed campaign into pass rates, first-failure seeds and shrunk sequences.

**Relationships**

- *devsoak:* a separate binary for block device drivers. fstest assumes the device is correct; the only shared artefact is the fault shim. A driver-level suspicion raised by fstest is handed to devsoak with the device and geometry.
- *Phoenix:* fstest is Phoenix's acceptance suite, but Phoenix is not fstest's only target; running the same suite against FFS and PFS3 is how the suite itself is validated and how differences are documented.
- *amiga-ffs / amiga-pfs / amiga-fuse:* provide the host-implementation and reference-handler oracles. fstest adds nothing to those crates beyond a verify entry point.
- *AmiBake:* builds the emulator environments (OS version, handler, devices) that the nightly tier boots.
- *volamos:* the per-commit tier depends on its handler-hosting phase; until that exists, the per-commit tier runs layer 2 against the host crates only.

## Phases and open questions

1. **Prove the harness.** A deliberately small first release: the baseline and about twenty file, lock and examine packets in `fstest conform`; `fstest random` with the portable model only and a handful of scenarios (create/write/read/delete, rename, directory walk, truncate); the report format with verdicts and oracle classes; the quirks file with the entries this exercise produces. Run against FFS (V40 and V47) and PFS3-AIO. This tests the riskiest assumptions first (safe probing, model semantics, quirks reporting, reproducibility) and the findings decide how much of the full packet matrix and the fault system is worth building. Deliverable: a documented verdict matrix for the existing handlers, useful on its own.
2. **Fill out layers 1 and 2:** the remaining packet matrix, handler-specific model expectations, invalid operations, shrinking, packet mode and concurrency mode.
3. **Churn** and the resumable soak, with the quirks file extended from phases 1 and 2.
4. **Host driver** and the volamos per-commit tier; AmiBake environments for the nightly tier.
5. **Fault injection**: the Copperline disk fault policy (specified with devsoak), then the campaign runner with the narrow first set (drop, ignore-flush, cut) and host-side verify; reorder, stale and tear follow once recovery assertions hold.

**Open questions**

- [ ] Final name: `fstest` provisional; to be checked against Aminet and general use before release
- [ ] Console and non-file-system handlers (CON:, PIPE:, RAM:) are a separate scope decision, deferred until the file-system packet model is established
- [ ] Exact shape of the copperhf fault policy to propose to Copperline (the only control-protocol gap)
