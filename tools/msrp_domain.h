// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef MSRP_DOMAIN_H
#define MSRP_DOMAIN_H

#include <stdint.h>
#include <pthread.h>
#include "utils.h"
#include "avb_status.h"
#include "mrp_client.h"

#define MSRP_CLASS_A_ID     6
#define MSRP_CLASS_A_VLAN   2
#define MSRP_CLASS_A_PRIORITY 3

struct msrp_domain_state {
    int ok;
    int class_id;
    int priority;
    int vid;
};

void msrp_domain_init(void);

void msrp_domain_cleanup(void);

void my_domain_cb(const struct mrp_domain_ctx ctx);

int msrp_domain_register(int id, int priority, int vid);

int msrp_domain_join_vlan(int id, int priority, int vid);

struct msrp_domain_state msrp_domain_get_state(void);

#endif