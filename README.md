# stressfs

stressfs is a format-agnostic test harness for Amiga file system handlers. One Amiga
binary runs four layers — packet conformance against the AmigaDOS Manual, seeded random
operations against an in-memory model, long-running churn, and scripted fault injection
in Copperline — against any mounted volume, so it tests the new handler, FFS, PFS3 and SFS alike.
Every result carries a verdict and the oracle that produced it, every run is seeded and
reproducible across emulators and hardware, and known handler deviations are declared in
a quirks file rather than hidden.

See [docs/proposal.md](docs/proposal.md) and
[docs/implementation-plan.md](docs/implementation-plan.md) for the full design and plan.

## Status

Phase 1 ("prove the harness") is implemented end to end: `stressfs conform` (packet
conformance, table-driven), `stressfs random`/`replay` (seeded model-based fuzzing), the
JSON-lines report/verdict/oracle format, and the `stressfs.quirks` confidence-tiered quirks
file, all exercised under both volamos (per-commit CI) and Copperline (nightly, real
handlers). Milestones M0–M5 from
[docs/implementation-plan.md](docs/implementation-plan.md) are done.

The M5 verdict matrix — [docs/verdict-matrix.md](docs/verdict-matrix.md) — currently covers
**FFS V40** and **FFS V47**, both green under Copperline. **PFS3-AIO does not mount**
under this session's Copperline/ROM pairing (see
[ci/m5/pfs3-aio-status.md](ci/m5/pfs3-aio-status.md)): `pfs3aio`'s own disk-presence check
refuses the partition before stressfs gets a chance to run against it, regardless of
whether the handler is registered via an RDB-embedded FSHD entry or an explicit
`S:Mountlist` entry (both were tried) — so phase 1's own exit bar ("all three handlers")
isn't met yet, and this is the one open item blocking that gate.

stressfs has also been pointed, ad hoc, at **Phoenix** — the new from-scratch filesystem
under development in the sibling `~/src/unnamed-fs` repo — using the same
`S:Mountlist`-based mount recipe that was later tried (unsuccessfully) against PFS3-AIO.
Phoenix mounts and identifies cleanly, and the very first `stressfs conform` run against it
surfaced a real conformance gap: Phoenix's write path uses a volume-wide, not per-file,
single-writer lock, so a mutating packet on one file is refused with
`ERROR_OBJECT_IN_USE` while an unrelated file elsewhere on the volume has an open write
handle — tighter than FFS/PFS3/SFS's ordinary concurrent-writer semantics. That's being
worked on upstream in Phoenix, not in stressfs.
