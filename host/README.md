# host/

Empty stub through phase 1 (docs/implementation-plan.md section 1), exactly as
`unnamed-fs` kept `handler/` empty through its own phase 1.

This becomes `stressfs-drive`, a Rust cargo workspace (phase 4,
docs/proposal.md "Host side"): tier orchestration, emulator config generation
from the TOML templates, report collection, fault injection through
Copperline's scripting surface, and the campaign summariser. It is independent
of the `amiga/` CMake tree — no shared build system, same division as
`unnamed-fs/rust/`.

Nothing here until M6 is done and phase 4 opens.
