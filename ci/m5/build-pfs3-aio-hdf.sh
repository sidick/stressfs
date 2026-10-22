#!/usr/bin/env bash
# ci/m5/build-pfs3-aio-hdf.sh
#
# M5: builds a PFS3-AIO test HDF with amitools (rdbtool), embedding the
# real pfs3aio handler via `fsadd`. Kept and documented even though it
# does NOT currently produce a bootable PFS3 volume under Copperline --
# see ci/m5/pfs3-aio-status.md for the full story (the partition mounts
# as an IDE device but pfs3aio's own disk-presence check refuses it,
# isolated to be PFS3-specific and not a general Copperline IDE gap).
# Kept in `ci/` anyway because "scripted boot environments checked into
# ci/" is what M5 asks for even for a tier that is not yet working, and
# because this script is exactly what the next person debugging the
# PFS3 gap should start from.
#
# Needs ~/amifuse/pfs3aio (a real loadseg()ble pfs3aio handler binary;
# see ~/src/amiga-files/Filesystems/pfs3aio-3.1.lha for the original
# archive). amibake has no PFS3 recipe (see ci/m5/pfs3-aio-status.md),
# so this is the amitools fallback the task brief allows.
#
# Usage:
#   ci/m5/build-pfs3-aio-hdf.sh /path/to/pfs3-aio.hdf [pfs3aio-binary] [size]
set -eu

OUT="${1:?usage: build-pfs3-aio-hdf.sh OUT.hdf [pfs3aio-binary] [size]}"
HANDLER="${2:-$HOME/amifuse/pfs3aio}"
SIZE="${3:-20Mi}"

if [ ! -f "$HANDLER" ]; then
  echo "error: pfs3aio handler binary not found at $HANDLER" >&2
  exit 1
fi

rm -f "$OUT"
# rdb_cyls=4: the default (1 cylinder) RDB area has no room left once
# the ~58 KiB pfs3aio handler is embedded via fsadd ("no space in RDB
# left"); 4 cylinders at this geometry is comfortably enough.
rdbtool "$OUT" create "size=$SIZE" + init rdb_cyls=4 \
  + fsadd "$HANDLER" dostype=PFS3 version=19.2 \
  + add size=100% name=Work dostype=PFS3 bootable \
  + info
echo "== built $OUT (PFS3 handler embedded, partition unformatted -- see"
echo "   ci/m5/pfs3-aio-status.md: PFS3's own disk-presence check refuses"
echo "   this partition under Copperline, so it does not actually mount) =="
