/* stressfs/packet.h -- Amiga-only packet-sending helpers for the M1
 * conform engine (docs/implementation-plan.md step 1: "packet sending
 * (DoPkt wrappers, BSTR/BPTR helpers)"). Unlike conform.h, this header
 * touches dos.library types (BPTR, BSTR, struct MsgPort) and therefore
 * only compiles cross, for m68k-amigaos; it is never included by
 * amiga/lib or amiga/tests.
 *
 * Two things live here:
 *
 *   1. BSTR construction: a long-word-aligned buffer big enough for a
 *      BCPL BSTR (one length byte + up to 255 chars), with a helper
 *      that fills it from a C string and hands back the BPTR.
 *
 *   2. The MAIN table talks to handlers with plain DoPkt() (see
 *      stressfs_dopkt() below) -- the same, simple, synchronous call
 *      the proposal itself describes ("sends every documented DOS
 *      packet by hand (DoPkt())") and the one volamos's fake dos.library
 *      actually implements (volamos intercepts the DoPkt() LVO call
 *      directly and answers it synchronously in host code; it does
 *      NOT implement the real SendPkt()/WaitPkt() message-passing
 *      primitives DoPkt() is built on, so hand-rolling those -- which
 *      an earlier version of this file did, in pursuit of a per-packet
 *      watchdog -- crashes every run under volamos with "unhandled
 *      library call"). DoPkt() cannot be raced against a timeout from
 *      the same process: it blocks in WaitPort()/GetMsg() on the
 *      calling process's own message port, and nothing short of
 *      another signal being *expected* by that same wait will unblock
 *      it, which defeats the purpose (a signal-based "break" would
 *      just be ignored by WaitPort()'s retry loop). So the real
 *      table's hang protection is NOT per-packet; it is the outer
 *      script-level timeout around the whole `stressfs conform`
 *      invocation (see ci/copperline-m1.sh) -- a literal second
 *      process (the shell's own `timeout`/Copperline's own run
 *      duration cap) watching the whole run, exactly matching "a
 *      second process with a timeout" at the granularity M1 actually
 *      needs it (never continue past a possibly damaged handler, abort
 *      the run) rather than at the granularity of a single packet.
 *
 *   3. A SEPARATE mechanism -- stressfs_send_packet_watched(), built
 *      from SendPkt() + a dedicated reply port + a timer.device
 *      deadline racing it -- exists purely to DEMONSTRATE the
 *      per-packet hang-detection idea end to end, via
 *      stressfs_watchdog_selftest() and conform.c's
 *      `CONFORM SELFTESTHANG` mode. It deliberately targets a stub
 *      MsgPort nobody services (the simplest possible "stub handler"),
 *      never a real volume's handler port, so it is never on the path
 *      of a real conform run and never interacts with volamos's
 *      DoPkt-only dispatch at all.
 */
#ifndef STRESSFS_PACKET_H
#define STRESSFS_PACKET_H

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include "stressfs/conform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A long-word-aligned BCPL BSTR buffer: one length byte followed by up
 * to 255 characters. The union forces 4-byte alignment (required for a
 * valid BPTR -- BADDR()/MKBADDR() just shift by 2, so an unaligned
 * buffer produces a bad pointer) without relying on a
 * compiler-specific alignment attribute. */
typedef union {
    ULONG force_align;
    UBYTE bytes[256];
} stressfs_bstr_t;

/* Fills `b` from the NUL-terminated C string `s` (truncated to 255
 * chars if longer) and returns the BPTR to hand to a packet argument.
 * `s` may be NULL or empty, producing a valid zero-length BSTR (the
 * empty-path convention several packets rely on, e.g.
 * ACTION_LOCATE_OBJECT with dp_Arg2 "" meaning "the object dp_Arg1
 * already locks"). */
BPTR stressfs_bstr_set(stressfs_bstr_t *b, const char *s);

/* The engine's one and only path for talking to a real handler in the
 * main table: a thin wrapper over DoPkt() returning both result codes
 * as a pair, which is all stressfs_conform_evaluate() needs. See this
 * header's comment above for why DoPkt() -- not the watched send
 * below -- is what the real table uses. */
stressfs_pkt_result_t stressfs_dopkt(struct MsgPort *port, LONG action,
                                      LONG arg1, LONG arg2, LONG arg3,
                                      LONG arg4, LONG arg5);

/* The result of a watched packet send. `hung` is nonzero if the
 * watchdog deadline fired before any reply arrived -- in that case
 * `result` is meaningless (both fields zeroed) and the caller MUST
 * treat this as STRESSFS_VERDICT_INCONCLUSIVE and abort the run per
 * the proposal's safe-probing rules, never retry or continue. */
typedef struct {
    stressfs_pkt_result_t result;
    int hung;
} stressfs_watched_result_t;

/* Opens the shared timer.device unit used for every watchdog deadline
 * in a conform run. Call once at startup; returns 0 on success. On
 * failure (timer.device unavailable -- should not happen on any real
 * or emulated AmigaOS) the caller should abort rather than run
 * unwatched, per the proposal's "never send an unattributable packet"
 * spirit. */
int stressfs_watchdog_open(void);
void stressfs_watchdog_close(void);

/* Sends one packet to `port` with up to five arguments (mirroring
 * DoPkt()'s own arity) and waits for the reply, racing it against a
 * `timeout_secs` timer.device deadline. This is the engine's one and
 * only path for talking to a handler -- amiga/src/conform.c never
 * calls DoPkt() directly, so every packet in the table is watched. */
stressfs_watched_result_t stressfs_send_packet_watched(struct MsgPort *port,
                                                        LONG action,
                                                        LONG arg1, LONG arg2,
                                                        LONG arg3, LONG arg4,
                                                        LONG arg5,
                                                        unsigned timeout_secs);

/* Demonstrates the hang-detection path end to end without needing a
 * real misbehaving handler: creates a MsgPort that nothing will ever
 * service (no task owns a message loop reading from it -- the
 * simplest possible "stub handler": a mailbox nobody checks), sends it
 * a packet through the exact same stressfs_send_packet_watched() path
 * every real test uses, and returns whether the watchdog correctly
 * detected the hang. See amiga/src/conform.c's header comment for why
 * this -- rather than a second real OS process -- is the mechanism
 * documented here, and docs/implementation-plan.md M1's allowance for
 * "a stub handler" as an acceptable demonstration vehicle. */
int stressfs_watchdog_selftest(unsigned timeout_secs);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_PACKET_H */
