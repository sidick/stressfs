# Implementation Plan — stressfs

Working plan against the proposal of 2026-10-21 ([docs/proposal.md](proposal.md)). The
proposal remains the authority on verdicts, oracle classes, the four layers and the
Copperline fault design; this document makes the structural decisions needed to start and
expands phase 1 ("Prove the harness") into executable milestones. Everything here is
revisable, but build to these decisions until a reason appears.

**Toolchain decision (fixed):** stressfs uses the exact same compiler and build setup as
the sibling handler repo (`~/src/unnamed-fs`): the pinned public `amigadev/crosstools` Docker
image (`docker.io/amigadev/crosstools:m68k-amigaos-gcc10`, pinned by digest
`sha256:3120749ec13b18a8f52e6073a484463f46751d69d8a67eaae10397958c05954d` — the tag says
"gcc10" but the image actually ships `m68k-amigaos-gcc (AmigaDev) 16.2.1b`, i.e. the
AmigaPorts/bebbo GCC 16.2 line), driven through a CMake toolchain file copied from
that repo's `cmake/m68k-amigaos.toolchain.cmake` and adapted. The adaptation matters:
that repo's core is a freestanding library, while `stressfs` is a **hosted CLI binary** — it
links against dos.library/exec.library through the toolchain's startup objects and
amiga.lib stubs, takes ReadArgs arguments, and prints to a console. So stressfs keeps
`-m68020-60 -Os -noixemul -Wall -Wextra -Werror -fno-common` and `CMAKE_SYSTEM_NAME
Generic`, but drops the freestanding-only constraints (no `-ffreestanding`, no "no libc
in sources" rule) and lets CMake's try-compile link a real executable where that works.
Linking goes through `m68k-amigaos-gcc`/`m68k-amigaos-ld` (GNU binutils) — there is no
vlink in this image. The proposal's "also built with 6.5 and vbcc in CI" is a **secondary
CI matrix added later** (see M6/phase 2 notes); GCC 16.2 in the pinned container is the
one primary toolchain, and the first milestones use it exclusively.

## 0. Decisions on open questions (blocking structural choices)

| Question | Decision | Rationale |
| --- | --- | --- |
| Name | **stressfs**, matching the repo — settled, overriding the proposal's `fstest` working name (the proposal's Aminet-name-check open question is resolved by this choice). Binary and subcommand names are `stressfs conform` / `random` / `churn` / `replay` from day one so docs don't churn. | Repo and binary names drifting apart is its own source of confusion; the report schema carries a `harness` field regardless, so a rename never invalidates stored reports. |
| Primary toolchain | Pinned `amigadev/crosstools` image + CMake toolchain file, mirroring unnamed-fs exactly (same digest, same flag philosophy). | Explicit requirement; one container serves both repos, and every lesson learned in unnamed-fs's build (the gcc10-tag trap, no-vlink, try-compile behaviour) transfers for free. |
| GCC 6.5 / vbcc builds | Deferred to a dedicated milestone after phase 1's harness works (M6). Not required for M0–M5. | The point of the secondary builds is catching codegen- and compiler-dependent bugs in the *harness*; that is only meaningful once the harness has behaviour worth cross-checking. |
| Minimum OS | Runs on 2.04 (V37) and up per the proposal; version-gated tests skip themselves. CI tiers run V40 and V47 first because those are the FFS targets of M5. | — |
| Licence | **BSD 2-Clause**, matching devsoak per the proposal ("both ship under BSD 2-Clause as devsoak does"). | Fixed by the proposal. |
| Quirks file name | `stressfs.quirks`, same format and semantics as `devsoak.quirks` (match conditions, actions, confidence tiers, `-k ID`/`-K` overrides). | Fixed by the proposal; sharing the format means sharing the parser eventually. |

## 1. Repository structure

Single repo. Layout:

```
stressfs/
├── docs/
│   ├── proposal.md            # The product proposal (authority on behaviour)
│   ├── implementation-plan.md # This file
│   └── quirks.md              # GENERATED from stressfs.quirks at release — never hand-edited
├── amiga/                     # The Amiga-side C: the stressfs binary itself
│   ├── src/                   # main + subcommand dispatch (ReadArgs), conform/, random/,
│   │                          #   churn/ (phase 3), replay/
│   ├── lib/                   # the helper library from the proposal: packet sending
│   │                          #   (DoPkt wrappers, BSTR/BPTR helpers), seeded generator
│   │                          #   (host-portable PRNG), model, report writer, quirks parser
│   ├── include/stressfs/        # public headers for lib/ (other test tools may consume)
│   └── tests/                 # host-runnable C unit tests of the portable pieces
│                              #   (model, generator, quirks parser, report formatter)
├── host/                      # Rust: stressfs-drive orchestrator (phase 4) — empty stub
│   │                          #   with a README until then; cargo workspace when real
├── data/
│   └── stressfs.quirks          # The single-source quirks file, shipped in the archive
├── cmake/
│   └── m68k-amigaos.toolchain.cmake  # Copied from unnamed-fs, freestanding bits removed
├── ci/                        # Scripts: cross build in the pinned container, volamos runs
└── .github/workflows/
```

Notes:

- The split inside `amiga/` between `src/` and `lib/` is the proposal's "small helper
  library … usable by other test tools" made structural: `lib/` has no dependency on the
  CLI, report policy or subcommand code, and is where the *portable* code lives — the
  model, generator, quirks parser and report formatter compile and unit-test on the
  **host** as plain C (they touch no Amiga APIs), which is how they get ASan/UBSan
  coverage. Packet sending is Amiga-only and cross-compiles only.
- The seed/PRNG and the model live in `lib/` precisely because phase 4's Rust
  `stressfs-drive` and the host crates must reproduce the same operation stream from the
  same seed. The PRNG is a fixed, documented algorithm (e.g. xoshiro128\*\* with a
  documented seeding rule), not whatever libc provides — this is a correctness
  requirement, not a style choice.
- `host/` stays an empty stub (one README) until phase 4, exactly as unnamed-fs kept
  `handler/` empty through its phase 1. The Copperline disk-fault shim is specified
  jointly with devsoak and lives in Copperline's repo, not here.
- `docs/quirks.md` is generated from `data/stressfs.quirks` by a build step (a small
  host-side script in `ci/`), satisfying the proposal's "the published list can never
  drift from what the tool actually does".

## 2. Build tooling

- **Amiga binary: CMake** (≥ 3.25), cross-only for the `stressfs` executable:
  - `cmake/m68k-amigaos.toolchain.cmake` is copied from
    `unnamed-fs/cmake/m68k-amigaos.toolchain.cmake` with these edits: keep
    `CMAKE_SYSTEM_NAME Generic`, `CMAKE_SYSTEM_PROCESSOR m68k`, the compiler/ar/ranlib
    settings, the flags line `-m68020-60 -Os -noixemul -Wall -Wextra -Werror
    -fno-common`, and the `CMAKE_FIND_ROOT_PATH_MODE_*` settings. Change
    `CMAKE_TRY_COMPILE_TARGET_TYPE`: stressfs is a hosted binary, so first try letting
    CMake link its test executable normally (the toolchain's `-noixemul` startup +
    libnix can link a trivial `main()`); fall back to `STATIC_LIBRARY` only if that
    proves flaky in the container — record whichever in the file's header comment, as
    unnamed-fs's file documents its own choices.
  - Configure/build, always inside the pinned container:

    ```
    docker run --rm -v "$PWD":/work -w /work \
      docker.io/amigadev/crosstools@sha256:3120749ec13b18a8f52e6073a484463f46751d69d8a67eaae10397958c05954d \
      sh -c 'cmake -S amiga -B build-m68k \
               -DCMAKE_TOOLCHAIN_FILE=../cmake/m68k-amigaos.toolchain.cmake \
             && cmake --build build-m68k'
    ```

    A `ci/cross-build.sh` wraps exactly this so local and CI builds are identical,
    following the pattern of `unnamed-fs/ci/volamos-m68k-tests.sh`.
  - The toolchain file is **not** expected to exist on the host; compilation happens in
    the container only, same policy as unnamed-fs.
- **Host-side unit tests of `amiga/lib/`: the same CMake tree, host flavour.** As in
  `unnamed-fs/core/CMakeLists.txt`: `cmake -S amiga -B build-host
  [-DFST_SANITIZERS=ON]`, `ctest --test-dir build-host --output-on-failure`. When not
  cross-compiling, only `lib/`'s portable sources and `tests/` build (guarded by
  `if(NOT CMAKE_CROSSCOMPILING)` for the tests and a portable/Amiga source split for the
  library); ASan/UBSan default-on for the host flavour and forced off when
  cross-compiling, mirroring unnamed-fs's `PHX_SANITIZERS` logic. Amiga-API-touching
  sources (`DoPkt`, console I/O) are excluded from the host build rather than stubbed —
  the host build exists to test the portable logic, not to fake AmigaOS.
- **Running the cross build.** Compiling is not proving. The M0 exit criterion includes
  *executing* the binary:
  - Per-commit: `volamos` on the host runs the stressfs binary directly (it is a plain CLI
    hunk executable), the same way `unnamed-fs/ci/volamos-m68k-tests.sh` runs its m68k
    test suite (`volamos --cpu 68020 --stack 2M build-m68k/stressfs ...`). volamos's
    handler hosting (the per-commit tier of the proposal) is a phase 4 dependency; until
    then volamos runs give us startup, ReadArgs, console output and the baseline packets
    against volamos's own volume emulation.
  - Nightly (from M5): Copperline boots an AmiBake-style environment with real FFS/PFS3
    volumes mounted and runs `stressfs` from a Shell script; `ci/copperline-run.sh` owns
    that invocation.
- **Rust (`host/`)**: untouched until phase 4; when it lands it is a cargo workspace
  independent of CMake, same division as unnamed-fs's `rust/`.
- **CI (GitHub Actions)**, phase-1 jobs: host build + ctest (ASan/UBSan); m68k cross
  build in the pinned container + binary-size report (stressfs should stay well under
  100 KB; report size every commit so growth is visible, per unnamed-fs practice);
  volamos smoke run of the cross-built binary. GCC 6.5 and vbcc jobs are added at M6,
  compile-only at first.

## 3. Phase 1 milestones ("Prove the harness"), in dependency order

Phase 1's goal, from the proposal: a deliberately small release — baseline + ~20 file,
lock and examine packets in `stressfs conform`; `stressfs random` with the portable model
only and a handful of scenarios; the report format with verdicts and oracle classes; the
quirks file with the entries this exercise produces; run against FFS V40, FFS V47 and
PFS3-AIO. It tests the riskiest assumptions first: safe probing, model semantics, quirks
reporting, reproducibility.

- **M0 — Build tooling proven.** Repo layout above; toolchain file adapted from
  unnamed-fs; CMake host + cross flavours; CI with host-test, cross-build and size-report
  jobs. The deliverable binary is a hello-world `stressfs` that: parses a ReadArgs template
  (`CONFORM/S,RANDOM/S,SEED/N/K,VOLUME/K,...` skeleton), opens dos.library explicitly
  with a version check, sends `ACTION_IS_FILESYSTEM` via `DoPkt()` to a named volume's
  handler port and prints the reply — i.e. one real packet end-to-end, proving startup
  code, library linkage, BPTR/BSTR handling and packet plumbing in one shot.
  *Exit:* the binary cross-compiles and links in the pinned container via
  `ci/cross-build.sh`; runs under volamos (`volamos build-m68k/stressfs`) printing the
  `ACTION_IS_FILESYSTEM` result against a volamos volume; runs identically under
  Copperline against a real FFS volume (manual run is fine at M0); host ctest green on a
  first `lib/` unit test (the PRNG with known vectors).
- **M1 — Baseline + the first ~20 packets in `stressfs conform`.** The table-driven
  conformance engine: a static table of test entries (packet, arguments builder, expected
  `dp_Res1`/`dp_Res2`, manual section reference, minimum DOS version), executed in order
  with the proposal's safe-probing ladder — handler identification first (DosType from
  the DosList, version string from the segment list where present), then the baseline
  (`ACTION_IS_FILESYSTEM`, `ACTION_NIL`, `ACTION_CURRENT_VOLUME`, root lock + free), and
  only then the matrix. Initial coverage: the file/lock/examine set —
  `ACTION_LOCATE_OBJECT` (shared/exclusive, success and `ERROR_OBJECT_NOT_FOUND`),
  `ACTION_FREE_LOCK`, `ACTION_COPY_DIR`, `ACTION_PARENT`, `ACTION_SAME_LOCK`,
  `ACTION_EXAMINE_OBJECT`, `ACTION_EXAMINE_NEXT` (including `ERROR_NO_MORE_ENTRIES`),
  `ACTION_FINDINPUT`, `ACTION_FINDOUTPUT`, `ACTION_FINDUPDATE`, `ACTION_READ`,
  `ACTION_WRITE`, `ACTION_SEEK` (all three modes), `ACTION_END`, `ACTION_SET_FILE_SIZE`,
  `ACTION_DELETE_OBJECT` (including delete-while-locked → `ERROR_OBJECT_IN_USE`),
  `ACTION_RENAME_OBJECT`, `ACTION_CREATE_DIR`, `ACTION_INFO`/`ACTION_DISK_INFO`, plus
  the exclusive-lock-on-root-refused quirk check from the proposal's list. Each test also
  has its dos.library-wrapper twin where one exists (`Lock`/`Examine`/`Open`...), per the
  proposal. Watchdog: a second process with a timeout marks a hung test inconclusive and
  aborts the run. Version gating works (tests annotated below the running handler's DOS
  version report `unsupported`).
  *Exit:* `stressfs conform` runs the baseline + the table against a volamos volume and
  against FFS V40 under Copperline without crashing either; a deliberately-wrong
  expectation produces a `fail` line (negative test of the engine); a hang produces
  `inconclusive` and an abort, demonstrated with a stub handler or a breakpointed run.
- **M2 — Report format + verdict/oracle machinery.** The report writer in `lib/`: one
  line per test with verdict (`pass`/`fail`/`deviation`/`unsupported`/`inconclusive`),
  oracle class (`independent`/`model`/`correlated`), manual section, and applied quirks;
  machine-readable (one JSON object per line is the working choice — cheap to emit from
  C, trivial to consume from Rust later) *and* a human-readable Shell rendering; a header
  block carrying the proposal's mandatory run identity (harness version, handler name +
  version, volume geometry + DosEnvec, tier, seed) — a report line missing any of these
  is a harness bug by definition, so the writer asserts it. Output to console, file or
  serial per the proposal. Exit codes for scripting (0 all-pass, distinct codes for
  fail/inconclusive).
  *Exit:* M1's run emits the format; a host-side ctest validates the writer against
  golden files; a trivial `ci/` checker script parses a real report and computes the
  verdict counts (the seed of phase 4's summariser).
- **M3 — Quirks file + confidence tiers.** The `stressfs.quirks` parser in `lib/` (host
  unit-tested): match conditions (handler name, version range, Kickstart version),
  actions (skip a packet, downgrade a test, expect a deviation), confidence tiers where
  only `confirmed` applies automatically and `reported`/`suspected` need `-k ID`, and
  `-K` to ignore the file entirely. Applied quirks listed at the top of the report, and
  per-line with id, confidence and match reason. The `docs/quirks.md` generator script.
  Seed the file with the entries already known from the proposal (e.g. FFS ≤ V40
  `ACTION_FLUSH` replying before the write finishes) marked at the appropriate
  confidence.
  *Exit:* the same conform table run against FFS V40 with and without `-K` demonstrably
  changes verdicts (a quirk-covered deviation vs. a raw finding); host tests cover the
  parser including malformed files (refused loudly, never half-applied); `docs/quirks.md`
  regenerates byte-identically from the shipped file in CI.
- **M4 — `stressfs random`, portable model only.** The seeded generator + in-memory model
  from the proposal's layer 2, restricted to phase 1 scope: operations
  create/open-each-mode/write/read/seek/close/delete, rename within and across
  directories, `CreateDir`, directory walk via `ExNext`, `SetFileSize` grow and truncate.
  Portable expectations only (contents, sizes, existence, fold-insensitive name identity,
  basic error codes) — handler-specific expectations wait for phase 2. Content generated
  from seed + offset, never stored. Checks after each step (result code + `IoErr()` vs.
  model, read-back, listing equality, `fib_Size`), full-tree compare every N steps.
  Working-set measurement and the refuse-to-run-what-won't-fit rule. No shrinking yet
  (phase 2) — but on divergence the full operation log with the seed is the output, which
  `stressfs replay` (a thin subcommand that re-executes a logged sequence) can re-run.
  *Exit:* `SEED=n` reproduces the identical operation stream on host (generator unit
  test), under volamos, and under Copperline; a seeded run of ≥ 10k operations against
  FFS V47 completes with the model agreeing or produces a logged, replayable divergence;
  an intentionally-broken model expectation demonstrates divergence detection end to end.
- **M5 — The verdict matrix: FFS V40, FFS V47, PFS3-AIO.** The phase-1 deliverable. Run
  `stressfs conform` and `stressfs random` (a handful of fixed seeds plus a nightly random
  one) against all three handlers under Copperline, from scripted boot environments
  checked into `ci/` (AmiBake proper is phase 4; for now a documented Copperline config +
  prepared HDF images per handler is enough, generated from the harness's TOML templates
  per the proposal's reproducibility rules). Every finding becomes either a quirks entry
  (with confidence) or a bug report upstream; the matrix (handler × test × verdict ×
  oracle class) is committed under `docs/` as the documented deliverable the proposal
  promises, generated from the reports, not hand-written.
  *Exit:* the matrix document exists and regenerates from stored report files; all three
  handlers complete both subcommands with zero `inconclusive` verdicts caused by the
  harness itself; the quirks file contains the entries this exercise produced; nightly CI
  runs the Copperline tier green. **Phase 1 gate:** the findings here decide how much of
  the full packet matrix and the fault system is worth building, per the proposal.
- **M6 — Secondary toolchains (compile matrix).** Add CI jobs building `stressfs` with
  bebbo GCC 6.5 and vbcc (compile + link; execution under volamos where it works). This
  is deliberately after the phase-1 gate: the harness must have proven behaviour before
  cross-compiler differences are worth chasing. Flag philosophy: same warnings-as-errors
  intent, with per-compiler flag files rather than contorting one flag set to fit three
  compilers.
  *Exit:* three-compiler CI matrix green; any behavioural difference found between
  compilers is itself recorded (it is exactly the class of finding the matrix exists
  for).

## 4. Phases 2–5 (outline)

Later phases follow the proposal's numbering; each gets its own milestone breakdown when
its phase opens, informed by the phase-1 findings.

- **Phase 2 — Fill out layers 1 and 2.** The remaining packet matrix (link, metadata,
  notification, volume and control packets; the documented-quirks list: `ACTION_MORE_CACHE`
  result convention, 64-bit seek, `ExAll`/`ED_OWNER` + matcher hook, soft links,
  `ACTION_SET_FILE_SIZE` clamping); probe mode for undocumented packets behind the
  identity gate; handler-specific model expectations loaded from the quirks file; invalid
  operations in the generator; delta-debugging shrinker emitting regression scripts that
  feed back into the conform table; packet mode (same generator via raw `DoPkt()`) with
  the permitted-differences list; concurrency mode (2–3 processes, disjoint subtrees,
  then shared files with record locks). Exit shape: the layer-1 coverage statement
  (packet × version × outcome × state cells exercised) is reportable per run.
- **Phase 3 — Churn and soak (`stressfs churn`).** The generalised
  FileSystemsStressTest workload; between-phase invariants (free-space bound per format
  from the quirks file, `Info()` self-consistency, full `ExAll` walk); fragmentation
  report where the handler exposes extents; 10-minute nightly and 8-hour hardware tiers;
  resumable run state on a separate volume with its own checksum. Quirks file extended
  from phases 1–2 findings.
- **Phase 4 — Host driver and tiers.** `stressfs-drive` in `host/` (Rust): tier
  orchestration, emulator config generation from the TOML templates, report collection
  and the campaign summariser (pass rates, first-failure seeds, shrunk sequences); the
  volamos per-commit tier via its handler hosting (until that exists, layer 2 runs
  against the host crates directly, per the proposal); AmiBake environments for the
  nightly tier; end-of-run host-implementation verify hooks into amiga-ffs/amiga-pfs.
- **Phase 5 — Fault injection.** The Copperline `disk.fault.*` policy, specified jointly
  with devsoak and implemented in Copperline (see Risks); then the campaign runner,
  starting narrow — drop, ignore-flush, cut — with recovery assertions validated against
  a known-good handler before reorder, stale reads and torn writes are enabled.
  Verification always by the host implementation reading the image, never the
  possibly-damaged handler; seed-range campaigns, not single runs.

## 5. Risks to watch

- **Safe probing is a crash risk by design.** Handlers that switch on `dp_Type` without a
  default crash on unknown packets, and in phase 1 we are sending packets to three real
  handlers whose behaviour the harness exists to discover. Mitigations are structural,
  not optional: the baseline-first ladder, identity-gating, probe mode strictly opt-in,
  the log-before-send rule so a crash is attributable, and the watchdog's
  `inconclusive`-and-abort (never continue on a possibly damaged handler). M1's exit
  criteria deliberately include demonstrating the failure paths, not only the happy path
  — an untested watchdog is the same as no watchdog.
- **Quirks-file drift from the packet matrix.** The quirks file is the single source for
  what gets probed, skipped and downgraded; if entries reference packets or tests the
  table no longer contains (or vice versa), quirks silently stop applying and findings
  get mislabelled. Mitigation: a CI check cross-references every quirks entry against the
  conform table's test ids and fails on dangling references, and `docs/quirks.md`
  regeneration is a CI gate (byte-identical or the build fails), so the published list
  and the shipped file cannot diverge — same contract-by-construction idea as
  unnamed-fs's spec/layout-test coupling.
- **Correlated vs. independent oracles is easy to get wrong in early code.** The
  temptation is to tag verdicts with the oracle class as a string decided at the
  call site; the first refactor then mislabels a correlated result as independent and the
  report overstates what was shown — the exact failure the verdict system exists to
  prevent. Mitigation: the oracle class is a property of the *oracle object* in `lib/`,
  not a parameter of the check call; host unit tests from M2 assert that each oracle type
  can only ever emit its own class, and the report writer refuses a verdict with no
  oracle attached.
- **Copperline's disk fault policy is a cross-repo dependency.** Phase 5 depends on
  `disk.fault.set`/`clear`/`list`/`stats` landing in Copperline, specified jointly with
  devsoak — another team/repo and the proposal's one acknowledged control-protocol gap.
  This is now tracked as [CopperlineHQ/Copperline#809](https://github.com/CopperlineHQ/Copperline/pull/809)
  rather than purely speculative, but it is still an open PR in another repo, not merged
  API stressfs can build against — re-check its status and the final method/policy shape
  against the proposal's §"The one proposal" before phase 5 starts, since a PR can still
  change shape before landing. Mitigations: phases 1–4 are sequenced to be fully useful
  without it; and if it slips, a degraded interim exists (power-cut-only campaigns via the
  existing `state.save`/`machine.reset` surface, which needs nothing new) — explicitly
  marked as the narrow subset it is.
- **volamos tier fidelity.** Per-commit runs go through volamos's API-level emulation,
  whose packet behaviour is itself an implementation, not the manual. A conform pass
  under volamos is evidence the *harness* works, not that a handler does; the report's
  tier field exists so nobody quotes a volamos run as a handler verdict. Divergences
  between volamos and Copperline runs of the same seed are findings about volamos — handed
  to that project, and recorded in the quirks file like any other environment difference.
- **Scope creep past the phase-1 gate.** The proposal is explicit that phase 1's findings
  decide how much of the packet matrix and fault system is worth building. `host/` stays
  a stub, probe mode stays unwritten, and no layer-3/4 code lands until M5's matrix is
  delivered and read.
