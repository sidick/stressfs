/* amiga/src/packet.c -- see amiga/include/stressfs/packet.h for the
 * design rationale (BSTR helper, and why packets are sent via
 * SendPkt()+timer.device instead of DoPkt() so a hang is detectable).
 */

#include "stressfs/packet.h"

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

BPTR stressfs_bstr_set(stressfs_bstr_t *b, const char *s)
{
    size_t len = 0;

    if (s != NULL) {
        len = strlen(s);
        if (len > 255) {
            len = 255;
        }
    }

    b->bytes[0] = (UBYTE)len;
    if (len != 0) {
        memcpy(&b->bytes[1], s, len);
    }

    return MKBADDR(b->bytes);
}

stressfs_pkt_result_t stressfs_dopkt(struct MsgPort *port, LONG action,
                                      LONG arg1, LONG arg2, LONG arg3,
                                      LONG arg4, LONG arg5)
{
    stressfs_pkt_result_t r;
    SetIoErr(0);
    r.res1 = DoPkt(port, action, arg1, arg2, arg3, arg4, arg5);
    r.res2 = IoErr();
    return r;
}

/* Shared watchdog timer unit: one timer.device request, opened once
 * per process (stressfs_watchdog_open()) and reused for every watched
 * packet send -- opening/closing a device per packet would be needless
 * overhead and, worse, would leave a device open across a hang (see
 * below for why we never wait for a hung request to settle). */
static struct MsgPort *g_timerport = NULL;
static struct timerequest *g_timerreq = NULL;

int stressfs_watchdog_open(void)
{
    g_timerport = CreateMsgPort();
    if (g_timerport == NULL) {
        return -1;
    }

    g_timerreq = (struct timerequest *)CreateIORequest(g_timerport, sizeof(struct timerequest));
    if (g_timerreq == NULL) {
        DeleteMsgPort(g_timerport);
        g_timerport = NULL;
        return -1;
    }

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)g_timerreq, 0) != 0) {
        DeleteIORequest((struct IORequest *)g_timerreq);
        DeleteMsgPort(g_timerport);
        g_timerreq = NULL;
        g_timerport = NULL;
        return -1;
    }

    return 0;
}

void stressfs_watchdog_close(void)
{
    if (g_timerreq != NULL) {
        CloseDevice((struct IORequest *)g_timerreq);
        DeleteIORequest((struct IORequest *)g_timerreq);
        g_timerreq = NULL;
    }
    if (g_timerport != NULL) {
        DeleteMsgPort(g_timerport);
        g_timerport = NULL;
    }
}

stressfs_watched_result_t stressfs_send_packet_watched(struct MsgPort *port,
                                                        LONG action,
                                                        LONG arg1, LONG arg2,
                                                        LONG arg3, LONG arg4,
                                                        LONG arg5,
                                                        unsigned timeout_secs)
{
    stressfs_watched_result_t out;
    struct MsgPort *replyport;
    /* A hand-built StandardPacket (dos/dosextens.h documents this
     * exact layout -- see the amigados-rkrm skill's
     * direct-packet-communication.md). Allocated on the stack per the
     * RKRM's own CreatePacket() example being fine as long as this
     * function (or a callee) also waits for the reply -- which this
     * function does, except in the hang path, discussed below. */
    struct StandardPacket sp;
    ULONG replysig, timersig, waitmask, got;

    out.result.res1 = 0;
    out.result.res2 = 0;
    out.hung = 0;

    if (port == NULL || g_timerreq == NULL || g_timerport == NULL) {
        out.hung = 1;
        return out;
    }

    replyport = CreateMsgPort();
    if (replyport == NULL) {
        out.hung = 1;
        return out;
    }

    sp.sp_Msg.mn_Node.ln_Type = NT_MESSAGE;
    sp.sp_Msg.mn_Node.ln_Name = (char *)&sp.sp_Pkt;
    sp.sp_Msg.mn_Length = sizeof(sp.sp_Pkt);
    sp.sp_Pkt.dp_Link = &sp.sp_Msg;
    sp.sp_Pkt.dp_Port = replyport;
    sp.sp_Pkt.dp_Type = action;
    sp.sp_Pkt.dp_Res1 = 0;
    sp.sp_Pkt.dp_Res2 = 0;
    sp.sp_Pkt.dp_Arg1 = arg1;
    sp.sp_Pkt.dp_Arg2 = arg2;
    sp.sp_Pkt.dp_Arg3 = arg3;
    sp.sp_Pkt.dp_Arg4 = arg4;
    sp.sp_Pkt.dp_Arg5 = arg5;
    sp.sp_Pkt.dp_Arg6 = 0;
    sp.sp_Pkt.dp_Arg7 = 0;

    g_timerreq->tr_node.io_Command = TR_ADDREQUEST;
    g_timerreq->tr_node.io_Flags = 0;
    g_timerreq->tr_node.io_Error = 0;
    g_timerreq->tr_time.tv_secs = timeout_secs;
    g_timerreq->tr_time.tv_micro = 0;
    SendIO((struct IORequest *)g_timerreq);

    SendPkt(&sp.sp_Pkt, port, replyport);

    replysig = 1UL << replyport->mp_SigBit;
    timersig = 1UL << g_timerport->mp_SigBit;
    waitmask = Wait(replysig | timersig);
    (void)waitmask;

    got = 0;
    for (;;) {
        struct Message *m = GetMsg(replyport);
        if (m == NULL) {
            break;
        }
        got = 1;
        /* There is exactly one outstanding packet on this reply port
         * (this function never issues a second one before the first
         * settles), so the first message received here must be our
         * own packet reply. */
    }

    if (got) {
        /* Reply arrived before (or in the same Wait() as) the
         * deadline. Cancel the now-unneeded timer request before
         * reusing it on the next call. */
        if (!CheckIO((struct IORequest *)g_timerreq)) {
            AbortIO((struct IORequest *)g_timerreq);
        }
        WaitIO((struct IORequest *)g_timerreq);

        out.result.res1 = sp.sp_Pkt.dp_Res1;
        out.result.res2 = sp.sp_Pkt.dp_Res2;
    } else {
        /* The watchdog deadline fired first: no reply. This is the
         * inconclusive/hang path -- see packet.h's header comment for
         * why we do NOT wait any further for the handler. The timer
         * request itself already completed (that's the signal we
         * woke up on); reap its message so the IORequest is not left
         * marked busy for the next stressfs_watchdog_open() cycle
         * (there is none in a single run, but it keeps the request
         * object in a sane state if a caller inspected it).
         *
         * The packet we sent is now permanently orphaned in the
         * target handler's hands (or lost): dos.library's own
         * AbortPkt() is documented as non-functional (see
         * direct-packet-communication.md), so there is no supported
         * way to recall it. We accept this because the whole point of
         * "inconclusive -> abort" is that the caller (conform.c) stops
         * sending anything else to this handler and the process exits
         * shortly after, which is the same exposure any killed Amiga
         * process already has with an in-flight handler operation.
         */
        WaitIO((struct IORequest *)g_timerreq);
        out.hung = 1;
    }

    DeleteMsgPort(replyport);
    return out;
}

/* A task entry point is not used here -- see the header comment on
 * stressfs_watchdog_selftest() in packet.h: the "stub handler" is just
 * a MsgPort nobody services, not a second process/task. Nothing here
 * needs CreateNewProcTags() or a task body. */
int stressfs_watchdog_selftest(unsigned timeout_secs)
{
    struct MsgPort *stub;
    stressfs_watched_result_t r;
    int ok;

    stub = CreateMsgPort();
    if (stub == NULL) {
        return 0;
    }

    r = stressfs_send_packet_watched(stub, ACTION_NIL, 0, 0, 0, 0, 0, timeout_secs);
    ok = r.hung;

    /* We never service `stub` (that's the point), so the packet we
     * just sent to it is permanently enqueued there. Deliberately NOT
     * calling DeleteMsgPort(stub) here: it still has that message
     * linked into its list, and freeing a port out from under a
     * linked message is a worse-understood risk than just leaking one
     * small port for the remainder of this process's short life (the
     * process exits shortly after this self-test runs -- see
     * conform.c -- which reclaims it, same as any process-owned
     * resource it didn't explicitly free). */

    return ok;
}
