// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#include <string.h>
#include "msrp_listener.h"
#include "avb_log.h"
#include "stream_identity.h"
#include "avdecc_acmp.h"
#include "avdecc_probe.h"

extern uint64_t g_device_source_mac;
extern struct avb_stream_id_cfg g_streams[MAX_STREAMS];

uint64_t msrp_listener_expected_stream_id(int k)
{
    struct acmp_conn rx = acmp_rx_get(k);

    if (rx.bound && rx.stream_id && rx.stream_id != ~0ULL)
        return rx.stream_id;
    return k ? 0 : AVB_STREAM_ID(g_streams[k].peer_mac, k);
}

void msrp_listener_on_talker_declaration(const struct mrp_talker_ctx ctx)
{
    int matched = -1;

    avdecc_probe_kick_sid((const uint8_t *)ctx.stream_id);
    for (int k = 0; k < g_num_streams; k++) {
        uint8_t remote_talker_stream[8];
        uint64_t want = msrp_listener_expected_stream_id(k);

        if (!want)
            continue;
        uint64_to_array8(want, remote_talker_stream);
        if (memcmp(ctx.stream_id, remote_talker_stream, 8))
            continue;
        matched = k;

        pthread_mutex_lock(&g_status_lock);
        enum listener_rsv was = g_status.listener[k].state;
        pthread_mutex_unlock(&g_status_lock);

        if (ctx.mode == MRP_LEAVE) {
            if (was == LS_READY) {
                status_update_listener(k, LS_ASKING, 1);
                avb_log(L_MSRP, LOG_INFO, "listener[%d] ASKING: remote talker withdrew our stream", k);
            }
            continue;
        }
        struct acmp_conn rx = acmp_rx_get(k);
        if (rx.bound) {
            status_update_listener(k, LS_READY, 0);
            if (was != LS_READY)
                avb_log(L_MSRP, LOG_INFO, "listener[%d] READY: remote talker is advertising our stream (ACMP connected)", k);
        } else {
            avb_log(L_MSRP, LOG_DEBUG, "listener[%d] NOT READY: talker advertising but no ACMP connection", k);
        }
    }

    if (matched < 0)
        avb_log(L_MSRP, LOG_DEBUG,
                "talker declaration: stream %02x%02x%02x%02x%02x%02x%02x%02x vid %d %s (not ours)",
                (unsigned char)ctx.stream_id[0], (unsigned char)ctx.stream_id[1],
                (unsigned char)ctx.stream_id[2], (unsigned char)ctx.stream_id[3],
                (unsigned char)ctx.stream_id[4], (unsigned char)ctx.stream_id[5],
                (unsigned char)ctx.stream_id[6], (unsigned char)ctx.stream_id[7],
                ctx.vid, ctx.mode == MRP_LEAVE ? "leave" : "join");
}

static int msrp_listener_sid_user(const struct msrp_listener_ctx *ctx, int n,
                                  const uint8_t sid[8], int except)
{
    for (int j = 0; j < n; j++)
        if (j != except && !memcmp(ctx[j].ix_stream, sid, 8))
            return j;
    return -1;
}

void msrp_listener_follow(struct msrp_listener_ctx *ctx, int n)
{
    for (int k = 0; k < n; k++) {
        struct acmp_conn rx = acmp_rx_get(k);
        uint64_t want = msrp_listener_expected_stream_id(k);
        uint8_t want_b[8], old[8];

        uint64_to_array8(want, want_b);
        if (!memcmp(want_b, ctx[k].ix_stream, 8))
            continue;
        memcpy(old, ctx[k].ix_stream, 8);
        memcpy(ctx[k].ix_stream, want_b, 8);
        int user = array8_to_uint64(old) ? msrp_listener_sid_user(ctx, n, old, k) : -2;
        if (user == -2) {
            /* was IDLE: nothing declared to withdraw */
        } else if (user < 0) {
            send_leave(old);
            avb_log(L_MSRP, LOG_INFO, "listener[%d] withdraws %016llx (S-L, no other listener uses it)",
                    k, (unsigned long long)array8_to_uint64(old));
        } else {
            avb_log(L_MSRP, LOG_INFO, "listener[%d] keeps %016llx declared for listener[%d] (no S-L)",
                    k, (unsigned long long)array8_to_uint64(old), user);
        }
        if (!want) {
            status_update_listener(k, LS_IDLE, 0);
            avb_log(L_MSRP, LOG_INFO, "listener[%d] IDLE: no ACMP connection, nothing declared", k);
            continue;
        }
        avb_log(L_MSRP, LOG_INFO, "listener[%d] stream id -> %016llx (%s)",
                k, (unsigned long long)want, rx.bound ? "ACMP" : "startup guess");
        msrp_listener_start(&ctx[k]);
    }
}

void msrp_listener_leave_all(struct msrp_listener_ctx *ctx, int n)
{
    for (int k = 0; k < n; k++) {
        int j;

        if (!array8_to_uint64(ctx[k].ix_stream))
            continue;
        for (j = 0; j < k; j++)
            if (!memcmp(ctx[j].ix_stream, ctx[k].ix_stream, 8))
                break;
        if (j == k)
            send_leave(ctx[k].ix_stream);
    }
}

void msrp_listener_init(void)
{
}

void msrp_listener_cleanup(void)
{
}

void msrp_listener_ask(struct msrp_listener_ctx *ctx)
{
    (void)ctx;
    send_ready(ctx->ix_stream);
    ctx->last_ms = get_time_in_ms();
}

void msrp_listener_start(struct msrp_listener_ctx *ctx)
{
    ctx->attempts = 1;
    ctx->last_ms = get_time_in_ms();
    status_update_listener(ctx->idx, LS_ASKING, 1);
    avb_log(L_MSRP, LOG_INFO, "opening reservation: listener[%d] ask", ctx->idx);
    msrp_listener_ask(ctx);
}

void msrp_listener_tick(struct msrp_listener_ctx *ctx, struct avb_status *status, pthread_mutex_t *status_lock)
{
    uint64_t now = get_time_in_ms();
    int k = ctx->idx;
    enum listener_rsv ls;
    int la;

    STATUS_LOCK();
    ls = status->listener[k].state;
    la = status->listener[k].attempts;
    STATUS_UNLOCK();

    if (ls == LS_ASKING && now - ctx->last_ms > MSRP_RETRY_MS) {
        if (la < MSRP_MAX_ATTEMPTS) {
            STATUS_LOCK();
            la = ++status->listener[k].attempts;
            STATUS_UNLOCK();
            avb_log(L_MSRP, LOG_INFO,
                    "listener[%d]: no talker yet, re-ask (attempt %d/%d)",
                    k, la, MSRP_MAX_ATTEMPTS);
            msrp_listener_ask(ctx);
        } else {
            STATUS_LOCK();
            status->listener[k].state = LS_FAILED;
            STATUS_UNLOCK();
            ctx->last_ms = now;
            avb_log(L_MSRP, LOG_ERR,
                    "listener[%d] reservation FAILED after %d attempts -- breaker OPEN, cooldown %ds",
                    k, MSRP_MAX_ATTEMPTS, MSRP_COOLDOWN_S);
        }
    } else if (ls == LS_FAILED && now - ctx->last_ms > (uint64_t)MSRP_COOLDOWN_S * 1000) {
        STATUS_LOCK();
        status->listener[k].state = LS_ASKING;
        status->listener[k].attempts = 1;
        STATUS_UNLOCK();
        avb_log(L_MSRP, LOG_WARNING, "listener[%d] breaker half-open -- re-probing reservation", k);
        msrp_listener_ask(ctx);
    }
}
