// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2019 Ralf Beck
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#include "msrp_domain.h"
#include "avb_log.h"

static struct msrp_domain_state g_domain_state = { 0, 0, 0, 0 };

void msrp_domain_init(void)
{
    g_domain_state.ok = 0;
    g_domain_state.class_id = 0;
    g_domain_state.priority = 0;
    g_domain_state.vid = 0;
    avb_log(L_MSRP, LOG_DEBUG, "domain module initialized");
}

void msrp_domain_cleanup(void)
{
    avb_log(L_MSRP, LOG_DEBUG, "domain module cleaned up");
}

void my_domain_cb(const struct mrp_domain_ctx ctx)
{
    int ok;

    if (ctx.id != MSRP_CLASS_A_ID)
        return;

    ok = (ctx.mode != MRP_LEAVE);
    g_domain_state.ok = ok;
    g_domain_state.class_id = ctx.id;
    g_domain_state.priority = ctx.priority;
    g_domain_state.vid = ctx.vid;

    status_update_domain(ok, ctx.id, ctx.priority, ctx.vid);

    avb_log(L_MSRP, LOG_INFO,
            "domain %s: class %d prio %d vid %d",
            ok ? "registered" : "left", ctx.id, ctx.priority, ctx.vid);
}

int msrp_domain_register(int id, int priority, int vid)
{
    struct mrp_domain_attr attr;
    int rc;

    attr.id = id;
    attr.priority = priority;
    attr.vid = vid;

    rc = mrp_register_domain(&attr);
    avb_log(L_MSRP, LOG_DEBUG, "mrp_register_domain rc=%d", rc);

    if (rc == 0) {
        g_domain_state.ok = 1;
        g_domain_state.class_id = id;
        g_domain_state.priority = priority;
        g_domain_state.vid = vid;
        status_update_domain(1, id, priority, vid);
    }

    return rc;
}

int msrp_domain_join_vlan(int id, int priority, int vid)
{
    struct mrp_domain_attr attr;
    int rc;

    attr.id = id;
    attr.priority = priority;
    attr.vid = vid;

    rc = mrp_join_vlan(&attr);
    avb_log(L_MSRP, LOG_DEBUG, "mrp_join_vlan (MVRP) rc=%d", rc);

    return rc;
}

struct msrp_domain_state msrp_domain_get_state(void)
{
    return g_domain_state;
}