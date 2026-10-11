// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef GPTP_CLIENT_H
#define GPTP_CLIENT_H

#include <stdint.h>
#include <pthread.h>

struct avb_status;

void gptp_client_init(void);

void gptp_client_cleanup(void);

void gptp_poll(struct avb_status *status, pthread_mutex_t *status_lock,
               int *was_locked, int *was_synced, uint64_t *was_gm);

uint64_t gptp_get_grandmaster_id(void);

uint8_t gptp_get_domain_number(void);

int gptp_is_locked(void);

int gptp_is_synced(void);

int64_t gptp_get_offset_ns(void);

int gptp_is_up(void);

#endif