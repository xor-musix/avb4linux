// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef MSRP_TALKER_H
#define MSRP_TALKER_H

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

struct msrp_talker_ctx {
    int idx;
    uint8_t ox_stream[8];
    uint8_t ox_mac[6];
    int pktsz;
    int interval;
    int latency;
    int vid;
    int priority;
    
    uint64_t last_ms;
    int attempts;
};

void msrp_talker_init(void);
void msrp_talker_cleanup(void);

void msrp_talker_on_listener_declaration(const struct mrp_listener_ctx ctx);

void msrp_talker_advertise(struct msrp_talker_ctx *ctx);
void msrp_talker_start(struct msrp_talker_ctx *ctx);
void msrp_talker_tick(struct msrp_talker_ctx *ctx, struct avb_status *status, pthread_mutex_t *status_lock);

const char *msrp_talker_state_str(enum talker_state s);

#endif