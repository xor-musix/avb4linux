/******************************************************************************
 *
 *  Copyright (c) 2019, Ralf Beck
 *  Copyright (c) 2026, mnemonic powerplay <mnemonic@xor-musix.de>
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/

#include <sys/types.h>
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#include <endian.h>
#include <stdbool.h>

#include "avb_log.h"
#include "utils.h"
#include "stream_identity.h"
#include "avb_status.h"
#include "avdecc_adp.h"
#include "avdecc_aecp.h"
#include "avdecc_acmp.h"
#include "avdecc_probe.h"

extern int ifindex;
extern char MAC[6];
extern char DMAC[];
extern uint64_t own_mac_address;
extern struct avb_stream_id_cfg g_streams[MAX_STREAMS];

extern int sendMsg(int sock, char* buffer, int buflen);

extern struct jdksavdecc_eui64 entity_guid;

#define HEADER_OFFSET 14
#define MACLEN 6

static uint64_t g_controller_id = 0;

void avdecc_acmp_init(uint64_t entity_id, uint64_t controller_id, uint64_t grandmaster_id)
{
    (void)entity_id;
    (void)grandmaster_id;
    g_controller_id = controller_id;
}

static struct acmp_conn g_tx_conns[MAX_STREAMS] = {0};
static struct acmp_conn g_rx_conns[MAX_STREAMS] = {0};
static pthread_mutex_t  g_acmp_lock = PTHREAD_MUTEX_INITIALIZER;

#define ACMP_MAX_LISTENERS 16
struct acmp_listener {
    uint64_t entity_id;
    uint16_t unique_id;
};
static struct acmp_listener g_tx_listeners[MAX_STREAMS][ACMP_MAX_LISTENERS];

#define ACMP_CONNECT_TX_TIMEOUT_MS 2000
#define ACMP_FRAME_MAX             128

struct acmp_pending {
    int      active;
    int      retried;
    uint16_t our_seq;
    uint64_t deadline_ms;
    int      len;
    char     cmd[ACMP_FRAME_MAX];
};
static struct acmp_pending g_rx_pending[MAX_STREAMS];
static uint16_t g_acmp_seq = 0x8000;

static void acmp_send_to_talker(int sock, const char *cmd, int len,
                                uint8_t message_type, uint16_t seq)
{
    char buf[ACMP_FRAME_MAX];

    memcpy(buf, cmd, len);
    memcpy(buf, DMAC, MACLEN);
    memcpy(&buf[MACLEN], MAC, MACLEN);
    jdksavdecc_common_control_header_set_control_data(message_type, buf, HEADER_OFFSET);
    jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_SUCCESS, buf, HEADER_OFFSET);
    jdksavdecc_acmpdu_set_sequence_id(seq, buf, HEADER_OFFSET);
    sendMsg(sock, buf, len);
}

static void acmp_send_connect_rx_response(int sock, const char *cmd, int len,
                                          const char *tx_resp, uint8_t status)
{
    char buf[ACMP_FRAME_MAX];

    memcpy(buf, cmd, len);
    memcpy(buf, DMAC, MACLEN);
    memcpy(&buf[MACLEN], MAC, MACLEN);
    jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, buf, HEADER_OFFSET);
    jdksavdecc_common_control_header_set_status(status, buf, HEADER_OFFSET);
    if (tx_resp) {
        jdksavdecc_common_control_header_set_stream_id(
            jdksavdecc_common_control_header_get_stream_id(tx_resp, HEADER_OFFSET), buf, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_stream_dest_mac(
            jdksavdecc_acmpdu_get_stream_dest_mac(tx_resp, HEADER_OFFSET), buf, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_stream_vlan_id(
            jdksavdecc_acmpdu_get_stream_vlan_id(tx_resp, HEADER_OFFSET), buf, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_connection_count(
            jdksavdecc_acmpdu_get_connection_count(tx_resp, HEADER_OFFSET), buf, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_flags(
            jdksavdecc_acmpdu_get_flags(tx_resp, HEADER_OFFSET), buf, HEADER_OFFSET);
    }
    sendMsg(sock, buf, len);
}

void acmp_tick(int sock, uint64_t now_ms)
{
    for (int k = 0; k < g_num_streams; k++) {
        char cmd[ACMP_FRAME_MAX];
        int len = 0, resend = 0, timeout = 0;
        uint16_t seq = 0;

        pthread_mutex_lock(&g_acmp_lock);
        struct acmp_pending *p = &g_rx_pending[k];
        if (p->active && now_ms >= p->deadline_ms) {
            memcpy(cmd, p->cmd, p->len);
            len = p->len;
            if (!p->retried) {
                p->retried = 1;
                p->our_seq = seq = g_acmp_seq++;
                p->deadline_ms = now_ms + ACMP_CONNECT_TX_TIMEOUT_MS;
                resend = 1;
            } else {
                p->active = 0;
                timeout = 1;
            }
        }
        pthread_mutex_unlock(&g_acmp_lock);

        if (resend) {
            avb_log(L_AVDECC, LOG_WARNING, "ACMP CONNECT_TX uid=%d: no talker response, retry seq=%u",
                    k, seq);
            acmp_send_to_talker(sock, cmd, len, JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, seq);
        } else if (timeout) {
            avb_log(L_AVDECC, LOG_WARNING, "ACMP CONNECT_TX uid=%d: talker timeout -> CONNECT_RX_RESPONSE LISTENER_TALKER_TIMEOUT",
                    k);
            acmp_send_connect_rx_response(sock, cmd, len, NULL,
                                          JDKSAVDECC_ACMP_STATUS_LISTENER_TALKER_TIMEOUT);
        }
    }
}

struct acmp_conn acmp_tx_get(int idx)
{
    struct acmp_conn c;
    memset(&c, 0, sizeof c);
    if (idx < 0 || idx >= g_num_streams)
        return c;
    pthread_mutex_lock(&g_acmp_lock);
    c = g_tx_conns[idx];
    pthread_mutex_unlock(&g_acmp_lock);
    return c;
}

struct acmp_conn acmp_rx_get(int idx)
{
    struct acmp_conn c;
    memset(&c, 0, sizeof c);
    if (idx < 0 || idx >= g_num_streams)
        return c;
    pthread_mutex_lock(&g_acmp_lock);
    c = g_rx_conns[idx];
    pthread_mutex_unlock(&g_acmp_lock);
    return c;
}

static bool acmp_is_controller_bound(struct acmp_conn *table, int idx)
{
    return table[idx].bound && table[idx].source == 2;
}

static void acmp_bind(struct acmp_conn *table, int idx, int source,
                      uint64_t talker_entity_id, uint16_t talker_unique_id,
                      uint64_t listener_entity_id, uint16_t listener_unique_id,
                      uint64_t stream_id, uint64_t dest_mac, uint16_t flags)
{
    if (idx < 0 || idx >= g_num_streams)
        return;
    pthread_mutex_lock(&g_acmp_lock);
    struct acmp_conn *c = &table[idx];
    c->bound             = 1;
    c->source            = source;
    c->talker_entity_id  = talker_entity_id;
    c->talker_unique_id  = talker_unique_id;
    c->listener_entity_id = listener_entity_id;
    c->listener_unique_id = listener_unique_id;
    c->stream_id         = stream_id;
    c->dest_mac          = dest_mac;
    c->flags             = flags;
    c->connection_count  = 1;
    pthread_mutex_unlock(&g_acmp_lock);
}

static void acmp_unbind(struct acmp_conn *table, int idx)
{
    if (idx < 0 || idx >= g_num_streams)
        return;
    pthread_mutex_lock(&g_acmp_lock);
    memset(&table[idx], 0, sizeof(table[idx]));
    pthread_mutex_unlock(&g_acmp_lock);
}

static int acmp_tx_add_listener(int idx, int source, uint64_t talker_entity_id,
                                struct acmp_listener l, uint64_t stream_id,
                                uint64_t dest_mac, struct acmp_listener *evicted)
{
    struct acmp_listener *list = g_tx_listeners[idx];
    int i, n;

    memset(evicted, 0, sizeof *evicted);
    pthread_mutex_lock(&g_acmp_lock);
    struct acmp_conn *c = &g_tx_conns[idx];
    n = c->connection_count;
    for (i = 0; i < n; i++)
        if (list[i].entity_id == l.entity_id && list[i].unique_id == l.unique_id)
            break;
    if (i == n) {
        if (n == ACMP_MAX_LISTENERS) {
            *evicted = list[0];
            memmove(list, list + 1, (n - 1) * sizeof *list);
            n--;
        }
        list[n++] = l;
    }
    c->bound              = 1;
    c->source             = source;
    c->talker_entity_id   = talker_entity_id;
    c->talker_unique_id   = idx;
    c->listener_entity_id = l.entity_id;
    c->listener_unique_id = l.unique_id;
    c->stream_id          = stream_id;
    c->dest_mac           = dest_mac;
    c->flags              = 0;
    c->connection_count   = n;
    pthread_mutex_unlock(&g_acmp_lock);
    return n;
}

static int acmp_tx_remove_listener(int idx, struct acmp_listener l)
{
    struct acmp_listener *list = g_tx_listeners[idx];
    int i, n;

    pthread_mutex_lock(&g_acmp_lock);
    struct acmp_conn *c = &g_tx_conns[idx];
    n = c->connection_count;
    for (i = 0; i < n; i++)
        if (list[i].entity_id == l.entity_id && list[i].unique_id == l.unique_id)
            break;
    if (i == n) {
        pthread_mutex_unlock(&g_acmp_lock);
        return -1;
    }
    memmove(&list[i], &list[i + 1], (n - i - 1) * sizeof *list);
    if (--n == 0) {
        memset(c, 0, sizeof *c);
    } else {
        c->listener_entity_id = list[n - 1].entity_id;
        c->listener_unique_id = list[n - 1].unique_id;
        c->connection_count   = n;
    }
    pthread_mutex_unlock(&g_acmp_lock);
    return n;
}

static int acmp_tx_listener_at(int idx, int ci, struct acmp_listener *out)
{
    int ok;

    pthread_mutex_lock(&g_acmp_lock);
    ok = ci >= 0 && ci < g_tx_conns[idx].connection_count;
    if (ok)
        *out = g_tx_listeners[idx][ci];
    pthread_mutex_unlock(&g_acmp_lock);
    return ok;
}

static void acmp_tx_log_evicted(int idx, struct acmp_listener ev)
{
    if (ev.entity_id)
        avb_log(L_AVDECC, LOG_WARNING, "ACMP talker uid=%d: listener list full (%d), dropped the oldest %016llx:%u",
                idx, ACMP_MAX_LISTENERS, (unsigned long long)ev.entity_id, ev.unique_id);
}

static int acmp_tx_peer_entities(int idx, uint64_t *out, int max)
{
    uint64_t seen[ACMP_MAX_LISTENERS];
    int n = 0;

    pthread_mutex_lock(&g_acmp_lock);
    for (int i = 0; i < g_tx_conns[idx].connection_count; i++) {
        uint64_t e = g_tx_listeners[idx][i].entity_id;
        int j;

        for (j = 0; j < n && seen[j] != e; j++)
            ;
        if (j == n) {
            if (n < max)
                out[n] = e;
            seen[n++] = e;
        }
    }
    pthread_mutex_unlock(&g_acmp_lock);
    return n;
}

static void acmp_publish_status(int idx)
{
    struct acmp_conn tx = acmp_tx_get(idx);
    struct acmp_conn rx = acmp_rx_get(idx);
    uint64_t peers[STATUS_TX_PEERS];
    int n = acmp_tx_peer_entities(idx, peers, STATUS_TX_PEERS);

    status_update_conns(idx, tx.bound, tx.listener_entity_id, n, peers,
                        rx.bound, rx.talker_entity_id,
                        rx.bound && rx.stream_id != ~0ULL ? rx.stream_id : 0);
}

void acmp_conns_init(void)
{
    for (int i = 0; i < g_num_streams; i++) {
        acmp_unbind(g_tx_conns, i);
        acmp_unbind(g_rx_conns, i);
    }
    for (int i = 0; i < g_num_streams; i++)
        acmp_publish_status(i);
}

void acmp_render_summary(char *buf, size_t n)
{
    size_t off = 0;

    if (!buf || n == 0)
        return;
    buf[0] = '\0';

    pthread_mutex_lock(&g_acmp_lock);
    for (int i = 0; i < g_num_streams && off < n; i++) {
        const struct acmp_conn *tx = &g_tx_conns[i];
        const struct acmp_conn *rx = &g_rx_conns[i];
        int w;
        if (tx->bound || rx->bound)
            w = snprintf(buf + off, n - off,
                         "stream %d: TX feeding %u listener%s  RX %s\n", i,
                         tx->connection_count, tx->connection_count == 1 ? "" : "s",
                         rx->bound ? "receiving from a talker" : "no talker");
        else
            w = snprintf(buf + off, n - off, "stream %d: idle (no connections)\n", i);
        if (w < 0)
            break;
        off += (size_t)w;
    }
    pthread_mutex_unlock(&g_acmp_lock);

    buf[n - 1] = '\0';
}

void avdecc_acmp_connect(int sock)
{
    char buffer[128];
    struct jdksavdecc_acmpdu cmd;

    struct jdksavdecc_eui64 controller_guid, remote_guid, f_stream;
    struct jdksavdecc_eui48 f_mac;

    bool self_connect_enabled = false;
    const char *env_self_connect = getenv("AVB_SELF_CONNECT");
    if (env_self_connect && strcmp(env_self_connect, "1") == 0) {
        self_connect_enabled = true;
    }

    jdksavdecc_eui64_init_from_uint64(&controller_guid, g_controller_id);
    jdksavdecc_eui64_init_from_uint64(&remote_guid, g_streams[0].peer_entity_id);
    jdksavdecc_eui64_init_from_uint64(&f_stream, 0xFFFFFFFFFFFFFFFFULL);
    jdksavdecc_eui48_init_from_uint64(&f_mac, 0xFFFFFFFFFFFFULL);

    if (acmp_is_controller_bound(g_rx_conns, 0)) {
        struct acmp_conn rx = acmp_rx_get(0);
        avb_log(L_AVDECC, LOG_INFO, "listener 0 already bound by a controller to talker %016llx; not self-connecting",
                (unsigned long long)rx.talker_entity_id);
    } else {
        cmd.header.cd = 1;
        cmd.header.subtype = JDKSAVDECC_SUBTYPE_ACMP;
        cmd.header.sv = 0;
        cmd.header.version = 0;
        cmd.header.message_type = JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND;
        cmd.header.status = JDKSAVDECC_ACMP_STATUS_SUCCESS;
        cmd.header.control_data_length = 44;
        cmd.header.stream_id = f_stream;
        cmd.controller_entity_id = controller_guid;
        cmd.talker_entity_id = remote_guid;
        cmd.listener_entity_id = entity_guid;
        cmd.talker_unique_id = 0;
        cmd.listener_unique_id = 0;
        cmd.stream_dest_mac = f_mac;
        cmd.connection_count = 0;
        cmd.sequence_id = 1;
        cmd.flags = 0;
        cmd.stream_vlan_id = 0;
        cmd.reserved = 0;

        memcpy(buffer, DMAC, MACLEN);
        memcpy(&buffer[MACLEN], MAC, MACLEN);
        buffer[12] = 0x22; buffer[13] = 0xf0;
        jdksavdecc_acmpdu_write(&cmd, buffer, HEADER_OFFSET, sizeof(buffer));
        sendMsg(sock, buffer, 70);

        acmp_bind(g_rx_conns, 0, 1,
                  jdksavdecc_eui64_convert_to_uint64(&remote_guid), 0,
                  jdksavdecc_eui64_convert_to_uint64(&entity_guid), 0,
                  jdksavdecc_eui64_convert_to_uint64(&f_stream),
                  jdksavdecc_eui48_convert_to_uint64(&f_mac),
                  0);
    }

    if (!self_connect_enabled) {
        avb_log(L_AVDECC, LOG_INFO, "talker 0 self-connect disabled (AVB_SELF_CONNECT!=1); "
                "waiting for a controller to connect a listener");
    } else if (acmp_is_controller_bound(g_tx_conns, 0)) {
        struct acmp_conn tx = acmp_tx_get(0);
        avb_log(L_AVDECC, LOG_INFO, "talker 0 already bound by a controller to listener %016llx; not self-connecting",
                (unsigned long long)tx.listener_entity_id);
    } else {
        cmd.header.message_type = JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND;
        cmd.header.stream_id = f_stream;
        cmd.controller_entity_id = controller_guid;
        cmd.talker_entity_id = entity_guid;
        cmd.listener_entity_id = remote_guid;

        memcpy(buffer, DMAC, MACLEN);
        memcpy(&buffer[MACLEN], MAC, MACLEN);
        buffer[12] = 0x22; buffer[13] = 0xf0;
        jdksavdecc_acmpdu_write(&cmd, buffer, HEADER_OFFSET, sizeof(buffer));
        sendMsg(sock, buffer, 70);

        struct acmp_listener l = { jdksavdecc_eui64_convert_to_uint64(&remote_guid), 0 }, ev;
        acmp_tx_add_listener(0, 1, jdksavdecc_eui64_convert_to_uint64(&entity_guid), l,
                             jdksavdecc_eui64_convert_to_uint64(&f_stream),
                             jdksavdecc_eui48_convert_to_uint64(&f_mac), &ev);
        acmp_tx_log_evicted(0, ev);
    }
}

void avdecc_acmp_handle_command(int sock, char *buffer, int len)
{
    switch (jdksavdecc_common_control_header_get_control_data(buffer, HEADER_OFFSET))
    {
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND:
       {
         handle_acmp_connect_tx_command(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE:
       {
         handle_acmp_connect_tx_response(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND:
       {
         handle_acmp_disconnect_tx_command(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE:
       {
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND:
       {
         handle_acmp_get_tx_state_command(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE:
       {
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND:
       {
         handle_acmp_connect_rx_command(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE:
       {
         handle_acmp_connect_rx_response(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND:
       {
         handle_acmp_disconnect_rx_command(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE:
       {
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND:
       {
         handle_acmp_get_rx_state_command(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE:
       {
         handle_acmp_get_rx_state_response(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND:
       {
         handle_acmp_get_tx_connection_command(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE:
       {
         break;
       }
       default:
         break;
    }
}

void handle_acmp_connect_tx_command(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 talker_guid = jdksavdecc_acmpdu_get_talker_entity_id(buffer, HEADER_OFFSET);
    struct jdksavdecc_eui64 listener_guid = jdksavdecc_acmpdu_get_listener_entity_id(buffer, HEADER_OFFSET);
    uint16_t talker_unique_id = jdksavdecc_acmpdu_get_talker_unique_id(buffer, HEADER_OFFSET);
    uint16_t listener_unique_id = jdksavdecc_acmpdu_get_listener_unique_id(buffer, HEADER_OFFSET);

    if (jdksavdecc_eui64_compare(&talker_guid, &entity_guid))
    {
       return;
    }

    if (talker_unique_id >= g_num_streams)
    {
       memcpy(&buffer[MACLEN], MAC, MACLEN);
       jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, buffer, HEADER_OFFSET);
       jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_TALKER_UNKNOWN_ID, buffer, HEADER_OFFSET);
       sendMsg(sock, buffer, len);
       return;
    }

    uint64_t listener_entity_id = jdksavdecc_eui64_convert_to_uint64(&listener_guid);
    uint64_t talker_entity_id = jdksavdecc_eui64_convert_to_uint64(&talker_guid);

    struct acmp_listener l = { listener_entity_id, listener_unique_id }, ev;
    int count = acmp_tx_add_listener(talker_unique_id, 2, talker_entity_id, l,
                                     g_streams[talker_unique_id].stream_id,
                                     g_streams[talker_unique_id].dest_mac, &ev);

    acmp_publish_status(talker_unique_id);
    avdecc_probe_note_connect_tx(listener_entity_id, listener_unique_id, talker_unique_id);

    acmp_tx_log_evicted(talker_unique_id, ev);
    avb_log(L_AVDECC, LOG_INFO, "ACMP CONNECT_TX uid=%u peer=%016llx:%u (%d listener%s)",
            talker_unique_id, (unsigned long long)listener_entity_id, listener_unique_id,
            count, count == 1 ? "" : "s");

    memcpy(&buffer[MACLEN], MAC, MACLEN);

    jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, buffer, HEADER_OFFSET);
    jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_SUCCESS, buffer, HEADER_OFFSET);

    struct jdksavdecc_eui64 stream_id;
    jdksavdecc_eui64_init_from_uint64(&stream_id, g_streams[talker_unique_id].stream_id);
    jdksavdecc_common_control_header_set_stream_id(stream_id, buffer, HEADER_OFFSET);

    struct jdksavdecc_eui48 dst_mac;
    jdksavdecc_eui48_init_from_uint64(&dst_mac, g_streams[talker_unique_id].dest_mac);
    jdksavdecc_acmpdu_set_stream_dest_mac(dst_mac, buffer, HEADER_OFFSET);

    jdksavdecc_acmpdu_set_connection_count(count, buffer, HEADER_OFFSET);
    jdksavdecc_acmpdu_set_stream_vlan_id(2, buffer, HEADER_OFFSET);

    sendMsg(sock, buffer, len);
}

/* ACMP responses are terminal: never put one back on the wire (a duplicate breaks MOTU connections). */
void handle_acmp_connect_tx_response(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 controller_guid = jdksavdecc_acmpdu_get_controller_entity_id(buffer, HEADER_OFFSET);
    struct jdksavdecc_eui64 talker_guid     = jdksavdecc_acmpdu_get_talker_entity_id(buffer, HEADER_OFFSET);
    struct jdksavdecc_eui64 listener_guid   = jdksavdecc_acmpdu_get_listener_entity_id(buffer, HEADER_OFFSET);
    uint16_t talker_unique_id   = jdksavdecc_acmpdu_get_talker_unique_id(buffer, HEADER_OFFSET);
    uint16_t listener_unique_id = jdksavdecc_acmpdu_get_listener_unique_id(buffer, HEADER_OFFSET);
    uint8_t  status             = jdksavdecc_common_control_header_get_status(buffer, HEADER_OFFSET);

    uint64_t talker_entity_id   = jdksavdecc_eui64_convert_to_uint64(&talker_guid);
    uint64_t listener_entity_id = jdksavdecc_eui64_convert_to_uint64(&listener_guid);

    avb_log(L_AVDECC, LOG_DEBUG, "connect_tx_response: talker %llx listener %llx status %u",
            (unsigned long long)talker_entity_id,
            (unsigned long long)listener_entity_id, status);

    if (!jdksavdecc_eui64_compare(&listener_guid, &entity_guid) &&
        listener_unique_id < g_num_streams && len <= ACMP_FRAME_MAX) {
        uint16_t seq = jdksavdecc_acmpdu_get_sequence_id(buffer, HEADER_OFFSET);
        char cmd[ACMP_FRAME_MAX];
        int clen = 0;

        pthread_mutex_lock(&g_acmp_lock);
        struct acmp_pending *p = &g_rx_pending[listener_unique_id];
        if (p->active && p->our_seq == seq) {
            memcpy(cmd, p->cmd, p->len);
            clen = p->len;
            p->active = 0;
        }
        pthread_mutex_unlock(&g_acmp_lock);

        if (clen) {
            struct jdksavdecc_eui64 sid_e = jdksavdecc_common_control_header_get_stream_id(buffer, HEADER_OFFSET);
            struct jdksavdecc_eui48 dst_e = jdksavdecc_acmpdu_get_stream_dest_mac(buffer, HEADER_OFFSET);
            uint64_t sid = jdksavdecc_eui64_convert_to_uint64(&sid_e);
            uint64_t dst = jdksavdecc_eui48_convert_to_uint64(&dst_e);

            if (status == JDKSAVDECC_ACMP_STATUS_SUCCESS) {
                acmp_bind(g_rx_conns, listener_unique_id, 2,
                          talker_entity_id, talker_unique_id,
                          listener_entity_id, listener_unique_id,
                          sid, dst,
                          jdksavdecc_acmpdu_get_flags(buffer, HEADER_OFFSET));
                acmp_publish_status(listener_unique_id);
            }
            avb_log(L_AVDECC, LOG_INFO,
                    "ACMP CONNECT_TX_RESPONSE uid=%u talker=%016llx:%u status=%u stream=%016llx dest=%012llx -> CONNECT_RX_RESPONSE",
                    listener_unique_id, (unsigned long long)talker_entity_id, talker_unique_id,
                    status, (unsigned long long)sid, (unsigned long long)dst);
            acmp_send_connect_rx_response(sock, cmd, clen, buffer, status);
            return;
        }
    }

    if (jdksavdecc_eui64_convert_to_uint64(&controller_guid) != g_controller_id)
        return;
    if (jdksavdecc_eui64_compare(&listener_guid, &entity_guid))
        return;
    if (status != JDKSAVDECC_ACMP_STATUS_SUCCESS)
        return;
    if (listener_unique_id >= g_num_streams)
        return;

    struct jdksavdecc_eui64 stream_id = jdksavdecc_common_control_header_get_stream_id(buffer, HEADER_OFFSET);
    struct jdksavdecc_eui48 dst_mac   = jdksavdecc_acmpdu_get_stream_dest_mac(buffer, HEADER_OFFSET);

    acmp_bind(g_rx_conns, listener_unique_id, 2,
              talker_entity_id, talker_unique_id,
              listener_entity_id, listener_unique_id,
              jdksavdecc_eui64_convert_to_uint64(&stream_id),
              jdksavdecc_eui48_convert_to_uint64(&dst_mac),
              0);
    acmp_publish_status(listener_unique_id);

    avb_log(L_AVDECC, LOG_INFO, "ACMP CONNECT_TX_RESPONSE: listener uid=%u bound to talker=%016llx stream=%016llx",
            listener_unique_id, (unsigned long long)talker_entity_id,
            (unsigned long long)jdksavdecc_eui64_convert_to_uint64(&stream_id));
}

void handle_acmp_get_rx_state_command(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 listener_guid = jdksavdecc_acmpdu_get_listener_entity_id(buffer, HEADER_OFFSET);
    uint16_t listener_unique_id = jdksavdecc_acmpdu_get_listener_unique_id(buffer, HEADER_OFFSET);

    if (jdksavdecc_eui64_compare(&listener_guid, &entity_guid))
    {
        return;
    }

    if (listener_unique_id >= g_num_streams)
    {
        memcpy(&buffer[MACLEN], MAC, MACLEN);
        jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, buffer, HEADER_OFFSET);
        jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_LISTENER_UNKNOWN_ID, buffer, HEADER_OFFSET);
        sendMsg(sock, buffer, len);
        return;
    }

    memcpy(&buffer[MACLEN], MAC, MACLEN);
    jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, buffer, HEADER_OFFSET);

    struct acmp_conn conn = acmp_rx_get(listener_unique_id);
    if (conn.bound)
    {
        jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_SUCCESS, buffer, HEADER_OFFSET);

        struct jdksavdecc_eui64 stream_id;
        jdksavdecc_eui64_init_from_uint64(&stream_id, conn.stream_id);
        jdksavdecc_common_control_header_set_stream_id(stream_id, buffer, HEADER_OFFSET);

        struct jdksavdecc_eui64 talker_entity_id;
        jdksavdecc_eui64_init_from_uint64(&talker_entity_id, conn.talker_entity_id);
        jdksavdecc_acmpdu_set_talker_entity_id(talker_entity_id, buffer, HEADER_OFFSET);

        struct jdksavdecc_eui48 dest_mac;
        jdksavdecc_eui48_init_from_uint64(&dest_mac, conn.dest_mac);
        jdksavdecc_acmpdu_set_stream_dest_mac(dest_mac, buffer, HEADER_OFFSET);

        jdksavdecc_acmpdu_set_connection_count(conn.connection_count, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_stream_vlan_id(conn.flags & 0xFF, buffer, HEADER_OFFSET);
    }
    else
    {
        jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_SUCCESS, buffer, HEADER_OFFSET);

        struct jdksavdecc_eui64 zero_id;
        struct jdksavdecc_eui48 zero_mac;
        jdksavdecc_eui64_init_from_uint64(&zero_id, 0);
        jdksavdecc_eui48_init_from_uint64(&zero_mac, 0);

        jdksavdecc_common_control_header_set_stream_id(zero_id, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_talker_entity_id(zero_id, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_stream_dest_mac(zero_mac, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_connection_count(0, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_stream_vlan_id(0, buffer, HEADER_OFFSET);
    }

    avb_log(L_AVDECC, LOG_DEBUG, "ACMP GET_RX_STATE uid=%u -> %s", listener_unique_id, conn.bound ? "bound" : "empty");

    sendMsg(sock, buffer, len);
}

void handle_acmp_get_tx_state_command(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 talker_guid = jdksavdecc_acmpdu_get_talker_entity_id(buffer, HEADER_OFFSET);
    uint16_t talker_unique_id = jdksavdecc_acmpdu_get_talker_unique_id(buffer, HEADER_OFFSET);

    if (talker_unique_id >= g_num_streams)
    {
        memcpy(&buffer[MACLEN], MAC, MACLEN);
        jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, buffer, HEADER_OFFSET);
        jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_TALKER_UNKNOWN_ID, buffer, HEADER_OFFSET);
        sendMsg(sock, buffer, len);
        return;
    }

    if (jdksavdecc_eui64_compare(&talker_guid, &entity_guid))
    {
        return;
    }

    memcpy(&buffer[MACLEN], MAC, MACLEN);
    jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, buffer, HEADER_OFFSET);

    struct acmp_conn conn = acmp_tx_get(talker_unique_id);
    if (conn.bound)
    {
        jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_SUCCESS, buffer, HEADER_OFFSET);

        struct jdksavdecc_eui64 stream_id;
        jdksavdecc_eui64_init_from_uint64(&stream_id, conn.stream_id);
        jdksavdecc_common_control_header_set_stream_id(stream_id, buffer, HEADER_OFFSET);

        struct jdksavdecc_eui64 listener_entity_id;
        jdksavdecc_eui64_init_from_uint64(&listener_entity_id, conn.listener_entity_id);
        jdksavdecc_acmpdu_set_listener_entity_id(listener_entity_id, buffer, HEADER_OFFSET);

        struct jdksavdecc_eui48 dest_mac;
        jdksavdecc_eui48_init_from_uint64(&dest_mac, conn.dest_mac);
        jdksavdecc_acmpdu_set_stream_dest_mac(dest_mac, buffer, HEADER_OFFSET);

        jdksavdecc_acmpdu_set_connection_count(conn.connection_count, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_stream_vlan_id(conn.flags & 0xFF, buffer, HEADER_OFFSET);
    }
    else
    {
        jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_SUCCESS, buffer, HEADER_OFFSET);

        struct jdksavdecc_eui64 zero_id;
        struct jdksavdecc_eui48 zero_mac;
        jdksavdecc_eui64_init_from_uint64(&zero_id, 0);
        jdksavdecc_eui48_init_from_uint64(&zero_mac, 0);

        jdksavdecc_common_control_header_set_stream_id(zero_id, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_talker_entity_id(zero_id, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_stream_dest_mac(zero_mac, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_connection_count(0, buffer, HEADER_OFFSET);
        jdksavdecc_acmpdu_set_stream_vlan_id(0, buffer, HEADER_OFFSET);
    }

    avb_log(L_AVDECC, LOG_DEBUG, "ACMP GET_TX_STATE uid=%u -> %s", talker_unique_id, conn.bound ? "bound" : "empty");

    sendMsg(sock, buffer, len);
}

void handle_acmp_get_tx_connection_command(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 talker_guid = jdksavdecc_acmpdu_get_talker_entity_id(buffer, HEADER_OFFSET);
    uint16_t talker_unique_id = jdksavdecc_acmpdu_get_talker_unique_id(buffer, HEADER_OFFSET);
    /* The connection index travels in the connection_count field. */
    uint16_t connection_index = jdksavdecc_acmpdu_get_connection_count(buffer, HEADER_OFFSET);

    if (talker_unique_id >= g_num_streams)
    {
        memcpy(&buffer[MACLEN], MAC, MACLEN);
        jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, buffer, HEADER_OFFSET);
        jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_TALKER_UNKNOWN_ID, buffer, HEADER_OFFSET);
        sendMsg(sock, buffer, len);
        return;
    }

    if (jdksavdecc_eui64_compare(&talker_guid, &entity_guid))
    {
        return;
    }

    memcpy(&buffer[MACLEN], MAC, MACLEN);
    jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, buffer, HEADER_OFFSET);

    struct acmp_conn conn = acmp_tx_get(talker_unique_id);
    if (!conn.bound)
    {
        jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_NOT_CONNECTED, buffer, HEADER_OFFSET);
        sendMsg(sock, buffer, len);
        return;
    }

    struct acmp_listener l;
    if (!acmp_tx_listener_at(talker_unique_id, connection_index, &l))
    {
        jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_NO_SUCH_CONNECTION, buffer, HEADER_OFFSET);
        sendMsg(sock, buffer, len);
        return;
    }

    jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_SUCCESS, buffer, HEADER_OFFSET);

    struct jdksavdecc_eui64 listener_entity_id;
    jdksavdecc_eui64_init_from_uint64(&listener_entity_id, l.entity_id);
    jdksavdecc_acmpdu_set_listener_entity_id(listener_entity_id, buffer, HEADER_OFFSET);
    jdksavdecc_acmpdu_set_listener_unique_id(l.unique_id, buffer, HEADER_OFFSET);

    struct jdksavdecc_eui64 stream_id;
    jdksavdecc_eui64_init_from_uint64(&stream_id, conn.stream_id);
    jdksavdecc_common_control_header_set_stream_id(stream_id, buffer, HEADER_OFFSET);

    struct jdksavdecc_eui48 dest_mac;
    jdksavdecc_eui48_init_from_uint64(&dest_mac, conn.dest_mac);
    jdksavdecc_acmpdu_set_stream_dest_mac(dest_mac, buffer, HEADER_OFFSET);

    jdksavdecc_acmpdu_set_connection_count(conn.connection_count, buffer, HEADER_OFFSET);
    jdksavdecc_acmpdu_set_stream_vlan_id(conn.flags & 0xFF, buffer, HEADER_OFFSET);

    avb_log(L_AVDECC, LOG_DEBUG, "ACMP GET_TX_CONNECTION uid=%u idx=%u -> %s", talker_unique_id, connection_index, conn.bound ? "success" : "error");

    sendMsg(sock, buffer, len);
}

void handle_acmp_connect_rx_command(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 listener_guid = jdksavdecc_acmpdu_get_listener_entity_id(buffer, HEADER_OFFSET);
    struct jdksavdecc_eui64 talker_guid = jdksavdecc_acmpdu_get_talker_entity_id(buffer, HEADER_OFFSET);
    uint16_t talker_unique_id = jdksavdecc_acmpdu_get_talker_unique_id(buffer, HEADER_OFFSET);
    uint16_t listener_unique_id = jdksavdecc_acmpdu_get_listener_unique_id(buffer, HEADER_OFFSET);

    if (jdksavdecc_eui64_compare(&listener_guid, &entity_guid))
    {
       return;
    }

    if (listener_unique_id >= g_num_streams)
    {
       memcpy(&buffer[MACLEN], MAC, MACLEN);
       jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, buffer, HEADER_OFFSET);
       jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_LISTENER_UNKNOWN_ID, buffer, HEADER_OFFSET);
       sendMsg(sock, buffer, len);
       return;
    }

    if (len > ACMP_FRAME_MAX)
       return;

    uint64_t talker_entity_id = jdksavdecc_eui64_convert_to_uint64(&talker_guid);
    uint16_t seq = 0;
    int send = 0;

    avdecc_probe_kick(talker_entity_id);

    pthread_mutex_lock(&g_acmp_lock);
    struct acmp_pending *p = &g_rx_pending[listener_unique_id];
    memcpy(p->cmd, buffer, len);
    p->len = len;
    if (!p->active) {
       p->active = 1;
       p->retried = 0;
       p->our_seq = seq = g_acmp_seq++;
       p->deadline_ms = get_time_in_ms() + ACMP_CONNECT_TX_TIMEOUT_MS;
       send = 1;
    }
    pthread_mutex_unlock(&g_acmp_lock);

    if (!send)
       return;

    avb_log(L_AVDECC, LOG_INFO, "ACMP CONNECT_RX uid=%u peer=%016llx:%u -> CONNECT_TX seq=%u",
            listener_unique_id, (unsigned long long)talker_entity_id, talker_unique_id, seq);
    acmp_send_to_talker(sock, buffer, len, JDKSAVDECC_ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, seq);
}

void handle_acmp_connect_rx_response(int sock, char *buffer, int len)
{
    (void)sock;
    (void)len;

    struct jdksavdecc_eui64 controller_guid = jdksavdecc_acmpdu_get_controller_entity_id(buffer, HEADER_OFFSET);
    struct jdksavdecc_eui64 talker_guid     = jdksavdecc_acmpdu_get_talker_entity_id(buffer, HEADER_OFFSET);
    struct jdksavdecc_eui64 listener_guid   = jdksavdecc_acmpdu_get_listener_entity_id(buffer, HEADER_OFFSET);
    uint16_t talker_unique_id   = jdksavdecc_acmpdu_get_talker_unique_id(buffer, HEADER_OFFSET);
    uint16_t listener_unique_id = jdksavdecc_acmpdu_get_listener_unique_id(buffer, HEADER_OFFSET);
    uint8_t  status             = jdksavdecc_common_control_header_get_status(buffer, HEADER_OFFSET);

    if (jdksavdecc_eui64_convert_to_uint64(&controller_guid) != g_controller_id)
        return;
    if (jdksavdecc_eui64_compare(&talker_guid, &entity_guid))
        return;
    if (status != JDKSAVDECC_ACMP_STATUS_SUCCESS)
        return;
    if (talker_unique_id >= g_num_streams)
        return;

    struct acmp_listener l = { jdksavdecc_eui64_convert_to_uint64(&listener_guid), listener_unique_id }, ev;
    acmp_tx_add_listener(talker_unique_id, 2, jdksavdecc_eui64_convert_to_uint64(&talker_guid), l,
                         g_streams[talker_unique_id].stream_id,
                         g_streams[talker_unique_id].dest_mac, &ev);
    acmp_publish_status(talker_unique_id);
    acmp_tx_log_evicted(talker_unique_id, ev);

    avb_log(L_AVDECC, LOG_INFO, "ACMP CONNECT_RX_RESPONSE: talker uid=%u bound to listener=%016llx",
            talker_unique_id,
            (unsigned long long)jdksavdecc_eui64_convert_to_uint64(&listener_guid));
}

void handle_acmp_get_rx_state_response(int sock, char *buffer, int len)
{
    avdecc_probe_handle_acmp_response(sock, buffer, len);
}

void handle_acmp_disconnect_tx_command(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 talker_guid = jdksavdecc_acmpdu_get_talker_entity_id(buffer, HEADER_OFFSET);
    struct jdksavdecc_eui64 listener_guid = jdksavdecc_acmpdu_get_listener_entity_id(buffer, HEADER_OFFSET);
    uint16_t talker_unique_id = jdksavdecc_acmpdu_get_talker_unique_id(buffer, HEADER_OFFSET);
    uint16_t listener_unique_id = jdksavdecc_acmpdu_get_listener_unique_id(buffer, HEADER_OFFSET);

    if (jdksavdecc_eui64_compare(&talker_guid, &entity_guid))
    {
       return;
    }

    if (talker_unique_id >= g_num_streams)
    {
       memcpy(&buffer[MACLEN], MAC, MACLEN);
       jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, buffer, HEADER_OFFSET);
       jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_TALKER_UNKNOWN_ID, buffer, HEADER_OFFSET);
       sendMsg(sock, buffer, len);
       return;
    }

    struct acmp_conn conn = acmp_tx_get(talker_unique_id);
    if (!conn.bound)
    {
       memcpy(&buffer[MACLEN], MAC, MACLEN);
       jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, buffer, HEADER_OFFSET);
       jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_NOT_CONNECTED, buffer, HEADER_OFFSET);
       sendMsg(sock, buffer, len);
       return;
    }

    struct acmp_listener l = { jdksavdecc_eui64_convert_to_uint64(&listener_guid), listener_unique_id };
    int left = acmp_tx_remove_listener(talker_unique_id, l);
    avdecc_probe_kick(l.entity_id);

    if (left < 0) {
       left = conn.connection_count;
       avb_log(L_AVDECC, LOG_INFO, "ACMP DISCONNECT_TX uid=%u from %016llx:%u - not a listener, %d still connected",
               talker_unique_id, (unsigned long long)l.entity_id, l.unique_id, left);
    } else if (left > 0) {
       acmp_publish_status(talker_unique_id);
       avb_log(L_AVDECC, LOG_INFO, "ACMP DISCONNECT_TX uid=%u from %016llx:%u - %d listener%s left",
               talker_unique_id, (unsigned long long)l.entity_id, l.unique_id,
               left, left == 1 ? "" : "s");
    } else {
       acmp_publish_status(talker_unique_id);

       status_update_talker(talker_unique_id, TK_IDLE, 0, 0);
       avb_log(L_AVDECC, LOG_INFO, "ACMP DISCONNECT_TX uid=%u from %016llx:%u - last listener, talker reset to IDLE",
               talker_unique_id, (unsigned long long)l.entity_id, l.unique_id);
    }

    memcpy(&buffer[MACLEN], MAC, MACLEN);

    jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, buffer, HEADER_OFFSET);
    jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_SUCCESS, buffer, HEADER_OFFSET);

    struct jdksavdecc_eui64 stream_id;
    jdksavdecc_eui64_init_from_uint64(&stream_id, conn.stream_id);
    jdksavdecc_common_control_header_set_stream_id(stream_id, buffer, HEADER_OFFSET);

    struct jdksavdecc_eui48 dst_mac;
    jdksavdecc_eui48_init_from_uint64(&dst_mac, conn.dest_mac);
    jdksavdecc_acmpdu_set_stream_dest_mac(dst_mac, buffer, HEADER_OFFSET);

    jdksavdecc_acmpdu_set_connection_count(left, buffer, HEADER_OFFSET);
    jdksavdecc_acmpdu_set_stream_vlan_id(0, buffer, HEADER_OFFSET);

    sendMsg(sock, buffer, len);
}

void handle_acmp_disconnect_rx_command(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 listener_guid = jdksavdecc_acmpdu_get_listener_entity_id(buffer, HEADER_OFFSET);
    uint16_t listener_unique_id = jdksavdecc_acmpdu_get_listener_unique_id(buffer, HEADER_OFFSET);

    if (jdksavdecc_eui64_compare(&listener_guid, &entity_guid))
    {
       return;
    }

    if (listener_unique_id >= g_num_streams)
    {
       memcpy(&buffer[MACLEN], MAC, MACLEN);
       jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, buffer, HEADER_OFFSET);
       jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_LISTENER_UNKNOWN_ID, buffer, HEADER_OFFSET);
       sendMsg(sock, buffer, len);
       return;
    }

    struct acmp_conn conn = acmp_rx_get(listener_unique_id);
    if (!conn.bound)
    {
       pthread_mutex_lock(&g_acmp_lock);
       g_rx_pending[listener_unique_id].active = 0;
       pthread_mutex_unlock(&g_acmp_lock);
       memcpy(&buffer[MACLEN], MAC, MACLEN);
       jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, buffer, HEADER_OFFSET);
       jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_NOT_CONNECTED, buffer, HEADER_OFFSET);
       sendMsg(sock, buffer, len);
       return;
    }

    acmp_unbind(g_rx_conns, listener_unique_id);
    acmp_publish_status(listener_unique_id);

    if (len <= ACMP_FRAME_MAX) {
       uint16_t seq;

       pthread_mutex_lock(&g_acmp_lock);
       g_rx_pending[listener_unique_id].active = 0;
       seq = g_acmp_seq++;
       pthread_mutex_unlock(&g_acmp_lock);
       acmp_send_to_talker(sock, buffer, len, JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, seq);
       avb_log(L_AVDECC, LOG_INFO, "ACMP DISCONNECT_RX uid=%u -> DISCONNECT_TX seq=%u",
               listener_unique_id, seq);
    }

    status_update_listener(listener_unique_id, listener_unique_id ? LS_IDLE : LS_ASKING,
                           listener_unique_id ? 0 : 1);
    avb_log(L_AVDECC, LOG_INFO, "ACMP DISCONNECT_RX uid=%u - listener reset to %s", listener_unique_id,
            listener_unique_id ? "IDLE" : "ASKING");

    memcpy(&buffer[MACLEN], MAC, MACLEN);

    jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, buffer, HEADER_OFFSET);
    jdksavdecc_common_control_header_set_status(JDKSAVDECC_ACMP_STATUS_SUCCESS, buffer, HEADER_OFFSET);

    struct jdksavdecc_eui64 stream_id;
    jdksavdecc_eui64_init_from_uint64(&stream_id, conn.stream_id);
    jdksavdecc_common_control_header_set_stream_id(stream_id, buffer, HEADER_OFFSET);

    struct jdksavdecc_eui48 dst_mac;
    jdksavdecc_eui48_init_from_uint64(&dst_mac, conn.dest_mac);
    jdksavdecc_acmpdu_set_stream_dest_mac(dst_mac, buffer, HEADER_OFFSET);

    jdksavdecc_acmpdu_set_connection_count(0, buffer, HEADER_OFFSET);
    jdksavdecc_acmpdu_set_stream_vlan_id(0, buffer, HEADER_OFFSET);

    sendMsg(sock, buffer, len);
}

void handle_acmp_get_tx_connection_response(int sock, char *buffer, int len)
{
    memcpy(&buffer[MACLEN], MAC, MACLEN);
    sendMsg(sock, buffer, len);
}
