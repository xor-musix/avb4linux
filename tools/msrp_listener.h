// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef MSRP_LISTENER_H
#define MSRP_LISTENER_H

#include <stdint.h>
#include <pthread.h>
#include "utils.h"
#include "avb_status.h"
#include "mrp_client.h"

#define STATUS_LOCK()   pthread_mutex_lock(status_lock)
#define STATUS_UNLOCK() pthread_mutex_unlock(status_lock)

#define MSRP_RETRY_MS     3000
#define MSRP_MAX_ATTEMPTS 5
#define MSRP_COOLDOWN_S   30

struct msrp_listener_ctx {
    int idx;
    uint8_t ix_stream[8];
    uint8_t ix_mac[6];
    
    uint64_t last_ms;
    int attempts;
};

void msrp_listener_init(void);
void msrp_listener_cleanup(void);

void msrp_listener_on_talker_declaration(const struct mrp_talker_ctx ctx);

void msrp_listener_ask(struct msrp_listener_ctx *ctx);
void msrp_listener_start(struct msrp_listener_ctx *ctx);
void msrp_listener_tick(struct msrp_listener_ctx *ctx, struct avb_status *status, pthread_mutex_t *status_lock);

void msrp_listener_follow(struct msrp_listener_ctx *ctx, int n);
void msrp_listener_leave_all(struct msrp_listener_ctx *ctx, int n);

uint64_t msrp_listener_expected_stream_id(int k);

const char *msrp_listener_state_str(enum listener_rsv s);

#endif