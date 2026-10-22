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
