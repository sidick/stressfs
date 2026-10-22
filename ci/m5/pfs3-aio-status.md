# PFS3-AIO boot environment -- M5 status: NOT achieved

M5 (docs/implementation-plan.md section 3's M5 entry) asks for all three
handlers -- FFS V40, FFS V47, PFS3-AIO -- running under Copperline. FFS V40
and FFS V47 both work (see `ci/copperline-m5.sh` and
`docs/verdict-matrix.md`); PFS3-AIO does not, after real effort across two
independent HDF-construction paths. This file documents exactly what was
tried and where it stops, per the task's own instruction to "report honestly
if you can only get it partially working" -- this is that report.

## What was tried

**amibake** (`~/src/amibake`) has no PFS3 recipe (`ls ~/src/amibake/recipes`
has no `pfs3`/`pfs3aio` entry), so it was ruled out immediately as the task
brief anticipated it might be ("budget your time accordingly... this is the
tier most likely to need real engineering effort").

**Path 1: reuse `~/amifuse/pfs.hdf`.** amifuse ships a working PFS3 test
image (`rdbtool pfs.hdf info` shows a real embedded `pfs3aio` handler,
version 19.2, size 59120 -- matching `~/amifuse/pfs3aio` byte-for-byte --
inside a partition named `PDH0` with real data already on it, `used=188`
blocks). Booted under Copperline (`[ide] master = pfs.hdf`, Kickstart
3.2.2/V47, A1200 profile): the HDF mounts as an IDE device, Copperline logs
`ide: master .../pfs.hdf` and the two `HOSTFS` mounts start normally, but
**no VOLUME entry ever appears in the guest's DosList** for `PDH0`. stressfs
itself reports `no such volume "PDH0:"` and `handler="(unidentified)"`.
amifuse's own `pfs.hdf` was almost certainly built and only ever exercised
through amifuse's own FUSE-based pfs3aio driver on the host, not booted on
a real (or emulated) Amiga -- there is no evidence it was ever a verified
bootable RDB image, and the embedded handler segment or RDB metadata is
plausibly not in a form a real Kickstart's boot-time RDB scan can mount.

**Path 2: build a fresh PFS3 HDF with amitools.** `rdbtool` can lay out an
RDB, embed the real `~/amifuse/pfs3aio` handler via `fsadd`, and add a
bootable partition with `dostype=PFS3` (see
`ci/m5/build-pfs3-aio-hdf.sh`) -- this is the amitools fallback the task
brief allows when amibake has no recipe. The gap: **neither `rdbtool` nor
`xdftool` can write a PFS3 root block** (xdftool only understands OFS/FFS;
`xdftool ... format` with a PFS3 dostype is not a supported target, and the
PFS3-side tool for this, `PFS3/tools/pfsformat` from
`~/src/amiga-files/Filesystems/PFS3_53.lha`, has to run *on* a booted
Amiga). Running `pfsformat` (or AmigaDOS's own `Format` command, which
packet-wise reaches the same handler) against the freshly-built, unformatted
partition under Copperline consistently produces:

    No disk present
    in device Work

An isolation test nails this down precisely: changing the SAME partition on
the SAME HDF to `dostype=ffs` (an ordinary FFS "DOS1" partition, otherwise
byte-identical -- same RDB, same geometry, same IDE unit, same Copperline
config) and retrying `Format` gives the NORMAL, expected dialog instead --
`Not a DOS disk in device Work`, i.e. "I can see a disk here, it just isn't
initialised yet", exactly what Format is supposed to say before offering to
proceed. So this is not a general Copperline IDE-emulation limitation (FFS
on the identical unit works correctly); it is specific to the `pfs3aio`
handler's own disk-presence check on this RDB-embedded setup. Seeding the
partition's first long with the literal PFS3 dostype bytes
(`0x50465303`) before retrying made no difference, ruling out "the boot
block reads as all-zero garbage" as the cause.

## What was NOT determined

*Why* pfs3aio's disk-presence check fails here specifically was not root
caused. Plausible candidates, in rough order of likelihood, none
confirmed: a DosEnvec field PFS3 reads that generic FFS does not
(`rdbtool add`'s defaults were used throughout -- `num_buffer`, `mask`,
`max_transfer` were never tuned for PFS3's own expectations, unlike a real
HDToolBox-driven PFS3 install which sets some of these differently); a
unit-presence/media-change query PFS3 issues that this specific embedded
handler build does not get a usable answer to under Copperline's IDE
emulation particularly (as opposed to the SCSI-translation path FFS and
PFS3 both nominally also support); or something handler-build-specific to
the exact `pfs3aio` binary used (a `pfs3aio-custom` variant also ships in
the same lha archive and was not tried).

## What this means for M5's exit criteria

* "all three handlers complete both subcommands" -- **not met for
  PFS3-AIO**. FFS V40 and FFS V47 both are (see
  `docs/verdict-matrix.md`).
* No PFS3-AIO quirks entries were added, and none were removed: the
  existing `pfs3-aio-identity-unexpressible` placeholder entry in
  `data/stressfs.quirks` is left in place exactly as M3 wrote it (a
  deliberately-unmatchable entry documenting the identification gap),
  except that M5 DID add the general `dostype=` match key its own comment
  asked for -- see `data/stressfs.quirks`'s M5 section. The identification
  gap itself remains open for PFS3 specifically, because PFS3 never
  mounted long enough to identify in the first place.
* This is reported here as a genuine project finding, not filed as an
  upstream bug anywhere (per the task's own scope: "out of scope to
  actually file anywhere -- just note it in your final report"). The
  clearest next step, if phase 1's other findings justify continuing to
  pursue PFS3 coverage, is almost certainly to drive a REAL (or
  WinUAE/FS-UAE) Amiga through HDToolBox's own PFS3 install flow once,
  capture the resulting RDB's exact DosEnvec/partition parameters with
  `rdbtool pfs.hdf json`, and replicate those parameters exactly with
  `rdbtool`/`fsadd` rather than rdbtool's generic defaults -- or, more
  simply, track down a PFS3 test image from the Amiga community that is
  independently known to boot on real hardware or WinUAE, rather than
  amifuse's FUSE-oriented one.
