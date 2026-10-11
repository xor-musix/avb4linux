// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#include "msrp_talker.h"
#include "avb_log.h"
#include "stream_identity.h"
#include "avdecc_acmp.h"

extern uint64_t own_mac_address;
extern struct avb_stream_id_cfg g_streams[MAX_STREAMS];

void msrp_talker_init(void)
{
}

void msrp_talker_on_listener_declaration(const struct mrp_listener_ctx ctx)
{
    for (int k = 0; k < g_num_streams; k++) {
        uint8_t our_talker_stream[8];
        uint64_to_array8(g_streams[k].stream_id, our_talker_stream);
        if (memcmp(ctx.stream_id, our_talker_stream, 8) != 0)
            continue;

        pthread_mutex_lock(&g_status_lock);
        enum talker_state was = g_status.talker[k].state;
        pthread_mutex_unlock(&g_status_lock);

        if (ctx.substate == MSRP_LISTENER_READY) {
            struct acmp_conn tx = acmp_tx_get(k);
            if (tx.bound) {
                status_update_talker(k, TK_READY, 0, 1);
                if (was != TK_READY)
                    avb_log(L_MSRP, LOG_INFO, "talker[%d] READY: a listener reserved our stream (ACMP connected)", k);
            } else {
                avb_log(L_MSRP, LOG_DEBUG, "talker[%d] NOT READY: listener ready but no ACMP connection", k);
            }
        } else if (ctx.substate == MSRP_LISTENER_ASKFAILED) {
            status_update_talker(k, TK_ADVERTISING, 1, 0);
            avb_log(L_MSRP, LOG_WARNING, "talker[%d]: listener reported asking-failed, re-advertising", k);
        }
    }
}

void msrp_talker_cleanup(void)
{
}

void msrp_talker_advertise(struct msrp_talker_ctx *ctx)
{
    mrp_advertise_stream(ctx->ox_stream, ctx->ox_mac, ctx->pktsz, ctx->interval,
                         ctx->latency, ctx->vid, ctx->priority);
    ctx->last_ms = get_time_in_ms();
}

void msrp_talker_start(struct msrp_talker_ctx *ctx)
{
    ctx->attempts = 1;
    ctx->last_ms = get_time_in_ms();
    status_update_talker(ctx->idx, TK_ADVERTISING, 1, 0);
    avb_log(L_MSRP, LOG_INFO, "opening reservation: talker[%d] advertise", ctx->idx);
    msrp_talker_advertise(ctx);
}

void msrp_talker_tick(struct msrp_talker_ctx *ctx, struct avb_status *status, pthread_mutex_t *status_lock)
{
    uint64_t now = get_time_in_ms();
    int k = ctx->idx;
    enum talker_state ts;
    int ta;

    STATUS_LOCK();
    ts = status->talker[k].state;
    ta = status->talker[k].attempts;
    STATUS_UNLOCK();

    if (ts == TK_ADVERTISING && now - ctx->last_ms > MSRP_RETRY_MS) {
        if (ta < MSRP_MAX_ATTEMPTS) {
            STATUS_LOCK();
            ta = ++status->talker[k].attempts;
            STATUS_UNLOCK();
            avb_log(L_MSRP, LOG_INFO,
                    "talker[%d]: no listener yet, re-advertise (attempt %d/%d)",
                    k, ta, MSRP_MAX_ATTEMPTS);
            msrp_talker_advertise(ctx);
        } else {
            STATUS_LOCK();
            status->talker[k].state = TK_FAILED;
            STATUS_UNLOCK();
            ctx->last_ms = now;
            avb_log(L_MSRP, LOG_ERR,
                    "talker[%d] reservation FAILED after %d attempts -- breaker OPEN, cooldown %ds",
                    k, MSRP_MAX_ATTEMPTS, MSRP_COOLDOWN_S);
        }
    } else if (ts == TK_FAILED && now - ctx->last_ms > (uint64_t)MSRP_COOLDOWN_S * 1000) {
        STATUS_LOCK();
        status->talker[k].state = TK_ADVERTISING;
        status->talker[k].attempts = 1;
        STATUS_UNLOCK();
        avb_log(L_MSRP, LOG_WARNING, "talker[%d] breaker half-open -- re-probing reservation", k);
        msrp_talker_advertise(ctx);
    }
}

const char *msrp_talker_state_str(enum talker_state s)
{
    switch (s) {
        case TK_ADVERTISING: return "ADVERTISING";
        case TK_READY: return "READY";
        case TK_FAILED: return "FAILED";
        default: return "IDLE";
    }
}

const char *msrp_listener_state_str(enum listener_rsv s)
{
    switch (s) {
        case LS_ASKING: return "ASKING";
        case LS_READY: return "READY";
        case LS_ASKFAILED: return "ASK-FAILED";
        case LS_FAILED: return "FAILED";
        default: return "IDLE";
    }
}
