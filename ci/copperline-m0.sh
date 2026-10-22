#!/usr/bin/env bash
# ci/copperline-m0.sh
#
# M0 (docs/implementation-plan.md): run the cross-built stressfs binary
# under Copperline against a real FFS volume, headlessly and
# reproducibly. The implementation plan allows a manual run at M0
# ("manual run is fine at M0"), but a manual *windowed* session is not
# reproducible or scriptable — per the copperline skill, the reliable
# way to verify anything is a headless, unthrottled, deterministic run,
# never a manual window session. This script replaces that manual
# window session with one.
#
# It needs a real AmigaOS Kickstart ROM and an FFS-formatted HDF image,
# neither of which this repo can ship (licensed/personal assets). Point
# it at your own via env vars, or use ~/src/amibake to build an HDF +
# matching emulator config from a declarative manifest instead of
# supplying one by hand (see docs/manifest.md there) — not yet wired up
# here; M5 is where this gets a scripted, checked-in volume matrix
# (FFS V40/V47, PFS3-AIO) instead of one developer's local image.
#
# Usage:
#   COPPERLINE_ROM=/path/to/kickstart.rom \
#   COPPERLINE_HDF=/path/to/ffs-volume.hdf \
#   COPPERLINE_VOLUME=Work: \
#     ci/copperline-m0.sh
#
# Produces: build-m68k/copperline-m0.png — a screenshot of the CLI output
# (dp_Res1/dp_Res2 printed on screen), for a human to read, taken in a
# separate scheduled-screenshot run (see below for why this can't be the
# same run as the exit-code check); and the exit code of this script,
# which is the exit-on-return run's actual pass/fail gate.
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$ROOT/build-m68k/stressfs"
SCREENSHOT="$ROOT/build-m68k/copperline-m0.png"

ROM="${COPPERLINE_ROM:-$HOME/kick32.a1200.rom}"
HDF="${COPPERLINE_HDF:-$HOME/os_drive.hdf}"
VOLUME="${COPPERLINE_VOLUME:-Work:}"
SCREENSHOT_AT="${COPPERLINE_SCREENSHOT_AT:-7}"

if [ ! -f "$BINARY" ]; then
  echo "error: $BINARY not found — run ci/cross-build.sh first" >&2
  exit 1
fi
if [ ! -f "$ROM" ]; then
  echo "error: Kickstart ROM not found at $ROM (set COPPERLINE_ROM)" >&2
  exit 1
fi
if [ ! -f "$HDF" ]; then
  echo "error: FFS HDF not found at $HDF (set COPPERLINE_HDF, or build one with ~/src/amibake)" >&2
  exit 1
fi

CONFIG="$(mktemp -t copperline-m0-XXXXXX.toml)"
trap 'rm -f "$CONFIG"' EXIT

cat > "$CONFIG" <<EOF
rom = "$ROM"

[machine]
profile = "A1200"

[ide]
master = "$HDF"
EOF

# --exit-on-return ends the session the instant the return code lands,
# so a --screenshot-after scheduled for any later time never fires in
# that same run. Two separate, independently deterministic headless
# runs instead of one: a screenshot run for a human to read the actual
# printed dp_Res1/dp_Res2, and an exit-on-return run as the scriptable
# pass/fail gate. Both boot from the same config and seed, so they are
# the same scenario twice, not two different ones.
echo "== capturing stressfs output under Copperline ($VOLUME on $HDF) =="
copperline --config "$CONFIG" --noaudio \
  --run "$BINARY" --run-args "VOLUME $VOLUME" \
  --screenshot-after "$SCREENSHOT_AT" "$SCREENSHOT"
echo "== screenshot: $SCREENSHOT =="

echo "== running stressfs under Copperline against $VOLUME on $HDF (exit-code gate) =="
rc=0
copperline --config "$CONFIG" --noaudio \
  --run "$BINARY" --run-args "VOLUME $VOLUME" \
  --exit-on-return || rc=$?

echo "== stressfs exit code under Copperline: $rc =="
exit "$rc"
