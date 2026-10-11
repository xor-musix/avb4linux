// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#include "gptp_client.h"
#include "avb_log.h"
#include "utils.h"
#include "avb_status.h"
#include <stdlib.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define GPTP_SHM_NAME "/ptp"

struct gptp_time_data_prefix {
    int64_t     ml_phoffset;
    int64_t     ls_phoffset;
    long double ml_freqoffset;
    long double ls_freqoffset;
    uint64_t    local_time;
    uint8_t     gptp_grandmaster_id[8];
    uint8_t     gptp_domain_number;
    uint8_t     clock_identity[8];
    uint8_t     priority1;
    uint8_t     clock_class;
    uint16_t    offset_scaled_log_variance;
    uint8_t     clock_accuracy;
    uint8_t     priority2;
    uint8_t     domain_number;
    int8_t      log_sync_interval;
    int8_t      log_announce_interval;
    int8_t      log_pdelay_interval;
    uint16_t    port_number;
};

static const volatile struct gptp_time_data_prefix *g_gptp = NULL;
static uint8_t g_gptp_domain_number = 0;

static int64_t g_offset_min_ns = 0;
static int64_t g_offset_max_ns = 0;
static int64_t g_offset_sum_ns = 0;
static uint32_t g_offset_count = 0;

#define GPTP_SYNC_THRESHOLD_NS 1000000LL
#define GPTP_UNGATE_THRESHOLD_NS 800000LL

void gptp_client_init(void)
{
    int fd = shm_open(GPTP_SHM_NAME, O_RDONLY, 0);
    if (fd < 0)
    {
        avb_log(L_GPTP, LOG_WARNING, "gPTP SHM \"%s\" not available (%s); "
                "advertising self as grandmaster", GPTP_SHM_NAME, strerror(errno));
        return;
    }

    size_t map_len = sizeof(pthread_mutex_t) + sizeof(struct gptp_time_data_prefix);
    void *addr = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (addr == MAP_FAILED)
    {
        avb_log(L_GPTP, LOG_WARNING, "mmap of gPTP SHM failed (%s); "
                "advertising self as grandmaster", strerror(errno));
        return;
    }

    g_gptp = (const volatile struct gptp_time_data_prefix *)
             ((const char *)addr + sizeof(pthread_mutex_t));
}

void gptp_client_cleanup(void)
{
    if (g_gptp)
    {
        const volatile char *base = (const volatile char *)g_gptp - sizeof(pthread_mutex_t);
        munmap((void *)base, sizeof(pthread_mutex_t) + sizeof(struct gptp_time_data_prefix));
        g_gptp = NULL;
    }
}

void gptp_poll(struct avb_status *status, pthread_mutex_t *status_lock,
               int *was_locked, int *was_synced, uint64_t *was_gm)
{
    int      up     = (g_gptp != NULL);
    uint64_t gm     = up ? gptp_get_grandmaster_id() : 0;
    int      locked = (gm != 0);
    int64_t  off    = up ? g_gptp->ml_phoffset : 0;
    uint8_t  dom    = g_gptp_domain_number;
    int      prev_synced;
    int      synced;

    pthread_mutex_lock(status_lock);
    *was_locked = status->gptp_locked;
    *was_synced = status->gptp_synced;
    *was_gm     = status->gptp_gm;
    prev_synced = status->gptp_synced;
    if (!locked)
        synced = 0;
    else if (prev_synced)
        synced = llabs((long long)off) < GPTP_SYNC_THRESHOLD_NS;
    else
        synced = llabs((long long)off) < GPTP_UNGATE_THRESHOLD_NS;
    status->gptp_up = up;  status->gptp_locked = locked;  status->gptp_synced = synced;
    status->gptp_gm = gm;  status->gptp_offset_ns = off;  status->gptp_domain = dom;

    if (g_offset_count == 0) {
        g_offset_min_ns = off;
        g_offset_max_ns = off;
    }
    if (off < g_offset_min_ns) g_offset_min_ns = off;
    if (off > g_offset_max_ns) g_offset_max_ns = off;
    g_offset_sum_ns += off;
    g_offset_count++;

    status->gptp_offset_min_ns = g_offset_min_ns;
    status->gptp_offset_max_ns = g_offset_max_ns;
    status->gptp_offset_sum_ns = g_offset_sum_ns;
    status->gptp_offset_samples = g_offset_count;

    pthread_mutex_unlock(status_lock);

    if (locked && (!(*was_locked) || *was_gm != gm))
        avb_log(L_GPTP, LOG_INFO, "grandmaster selected: %016llx (domain %u)",
                (unsigned long long)gm, dom);
    else if (!locked && *was_locked)
        avb_log(L_GPTP, LOG_WARNING, "grandmaster lost (no gPTP master)");

    if (synced && !(*was_synced))
        avb_log(L_GPTP, LOG_INFO, "phase synced to grandmaster (offset %lldns)", (long long)off);
    else if (!synced && *was_synced)
        avb_log(L_GPTP, LOG_WARNING,
                "phase sync LOST (offset %lldns) -- PHC not aligned to grandmaster",
                (long long)off);
}

uint64_t gptp_get_grandmaster_id(void)
{
    if (!g_gptp)
        return 0;

    uint8_t gm_bytes[8];
    uint64_t gm;

    memcpy(gm_bytes, (const void *)g_gptp->gptp_grandmaster_id, sizeof(gm_bytes));
    gm = array8_to_uint64(gm_bytes);
    if (gm == 0)
        return 0;

    g_gptp_domain_number = g_gptp->gptp_domain_number;
    return gm;
}

uint8_t gptp_get_domain_number(void)
{
    return g_gptp_domain_number;
}

int gptp_is_locked(void)
{
    return gptp_get_grandmaster_id() != 0;
}

int gptp_is_synced(void)
{
    if (!g_gptp)
        return 0;
    int64_t off = g_gptp->ml_phoffset;
    return gptp_is_locked() && llabs((long long)off) < GPTP_SYNC_THRESHOLD_NS;
}

int64_t gptp_get_offset_ns(void)
{
    if (!g_gptp)
        return 0;
    return g_gptp->ml_phoffset;
}

int gptp_is_up(void)
{
    return (g_gptp != NULL);
}
