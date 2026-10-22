#!/usr/bin/env bash
# ci/m5/lib.sh
#
# M5 (docs/implementation-plan.md section 3's M5 entry): shared plumbing
# for "boot X under Copperline and run stressfs conform/random against
# it", factored out of ci/copperline-m0.sh through -m4.sh's copy-pasted
# pattern now that M5 needs it for THREE (rom, hdf, tier) triples
# instead of one. Sourced by ci/copperline-m5.sh and by
# ci/m5/run-tier.sh; not meant to be run directly.
#
# Same standing project rule as every earlier milestone's scripts:
# always a headless, scripted Copperline run (`--run`/`--exit-on-return`,
# a generated TOML config), never a manual windowed session.
set -eu

M5_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
M5_BINARY="$M5_ROOT/build-m68k/stressfs"
M5_QUIRKS_SRC="$M5_ROOT/data/stressfs.quirks"

# m5_run_once CONFIG_TOML LABEL RUN_ARGS... REPORT_DIR
#
# Runs one `stressfs <RUN_ARGS>` pass under Copperline with CONFIG_TOML,
# headless, `--exit-on-return`, under an outer `timeout` (the
# script-level watchdog M1's own header comment already documents: a
# genuinely hung handler hangs the whole process, and `timeout` kills
# it rather than letting a CI job hang forever). Copies both report
# files (conform's two, or random's two -- whichever the subcommand
# produced) into REPORT_DIR/<LABEL>.{txt,json} if they exist, and
# always copies data/stressfs.quirks next to the binary first (every
# earlier milestone's scripts do this; under `--run` the guest's
# current directory is RunProg:, the host directory holding the
# binary, which is the first entry in quirks_file.c's default search
# order).
#
# Sets M5_LAST_RC to the Copperline process's own exit code (124 = the
# outer timeout fired; see each caller for how it is expected to react
# -- a conform hang on a non-pristine volume is NOT the same finding
# as a handler hang on a pristine one, see ci/copperline-m5.sh's header
# comment on why every conform/random pass in this milestone always
# runs against a freshly-reformatted volume).
m5_run_once() {
  config_toml="$1"; shift
  label="$1"; shift
  report_dir="${!#}"
  run_args=("${@:1:$#-1}")

  if [ ! -f "$M5_BINARY" ]; then
    echo "error: $M5_BINARY not found -- run ci/cross-build.sh first" >&2
    return 1
  fi
  cp "$M5_QUIRKS_SRC" "$(dirname "$M5_BINARY")/stressfs.quirks"
  mkdir -p "$report_dir"

  rm -f "$(dirname "$M5_BINARY")"/stressfs-conform-report.* \
        "$(dirname "$M5_BINARY")"/stressfs-random-report.*

  timeout_secs="${M5_TIMEOUT_SECS:-120}"
  echo ""
  echo "== [$label] stressfs ${run_args[*]} =="
  rc=0
  timeout "$timeout_secs" copperline --config "$config_toml" --noaudio \
    --run "$M5_BINARY" --run-args "${run_args[*]}" \
    --exit-on-return || rc=$?
  M5_LAST_RC=$rc
  echo "== [$label] copperline exit code: $rc =="

  for kind in conform random; do
    txt="$(dirname "$M5_BINARY")/stressfs-$kind-report.txt"
    json="$(dirname "$M5_BINARY")/stressfs-$kind-report.json"
    if [ -s "$json" ]; then
      cp "$txt" "$report_dir/$label.txt" 2>/dev/null || true
      cp "$json" "$report_dir/$label.json"
    fi
  done
}

# m5_write_config OUT_PATH ROM EXTRA_TOML
# Writes a minimal Copperline config: top-level `rom`, then whatever
# machine/controller TOML the caller supplies (the [ide] or [scsi]
# section a given handler's boot environment needs -- see
# ci/copperline-m5.sh for why FFS V40's A3000-profile ROM needs
# [scsi] controller="a3000" rather than [ide]).
m5_write_config() {
  out="$1"; rom="$2"; extra="$3"
  {
    echo "rom = \"$rom\""
    echo ""
    echo "$extra"
  } > "$out"
}
