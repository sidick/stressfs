#!/usr/bin/env bash
# ci/m5/build-ffs-v40-hdf.sh
#
# M5: builds (or rebuilds, fresh/pristine) the FFS V40 test HDF.
#
# amibake (~/src/amibake) was evaluated first, per the task's standing
# instruction to prefer it for HDF construction. It has no AmigaOS 3.1
# base -- only os3.1.4 and os3.2.2 (see ~/src/amibake/recipes) -- and
# AmigaOS 3.1.4 updates FastFileSystem past V40 (it is one of 3.1.4's
# headline changes), so an amibake os3.1.4 image would not exercise
# FFS V40 at all. There is also no need for a full OS install here:
# FastFileSystem V40 is a ROM-resident module on an A3000/A4000-profile
# Kickstart 3.1 ROM (confirmed with `romtool scan
# ~/kicksmash32/amiga-os-310-a3000.rom`, which lists
# "filesystem  fs 40.1" as a NT_UNKNOWN resident), and Copperline's
# `--run` stages its own minimal boot volume independent of the IDE/SCSI
# drive under test (see docs/guide/run.md) -- so all the test volume
# needs is a valid FFS partition, which amitools builds directly and
# reproducibly:
#
#   rdbtool: lay out the RDB and one bootable partition, dostype=ffs
#            (DOS1, FFS international -- stressfs's own conform run
#            confirms this in its run-identity header either way, per
#            M5's instruction to verify identity rather than assume it)
#   xdftool: write the FFS root block (the part rdbtool does not do)
#
# This is the "fall back to amitools when amibake genuinely can't do
# it" path the task brief explicitly allows, documented here rather
# than left to be rediscovered.
#
# IMPORTANT: run this before EVERY `stressfs conform` pass against FFS
# V40, not just once. A second conform run against an already-exercised
# FFS V40 volume was observed to hang (Copperline's own outer `timeout`
# fired, not a per-packet watchdog) -- the M1 conform table's fixtures
# assume a pristine volume (see e.g. run_create_dir, run_findoutput),
# and FFS V40 was never exercised against a non-pristine run during
# M1-M4 (only FFS V47 was, and only ever via `stressfs random`'s own
# SEED-reproducibility passes, which reuse the SAME fixture path
# idempotently rather than conform's one-shot fixture set). This is
# recorded as a real, reproducible operational constraint on this
# script, not papered over: conform-table fixture idempotency across
# repeated runs is exactly the kind of thing M5's zero-harness-
# -inconclusive bar is meant to catch, and it is flagged in
# docs/verdict-matrix.md rather than silently worked around.
#
# Usage:
#   ci/m5/build-ffs-v40-hdf.sh /path/to/ffs-v40.hdf [size]
set -eu

OUT="${1:?usage: build-ffs-v40-hdf.sh OUT.hdf [size]}"
SIZE="${2:-20Mi}"

rm -f "$OUT"
rdbtool "$OUT" create "size=$SIZE" + init + add size=100% name=Work bootable dostype=ffs
xdftool "$OUT" open part=Work + format Work ffs
echo "== built $OUT (FFS DOS1, volume Work:) =="
