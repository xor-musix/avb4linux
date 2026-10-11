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
#include <pthread.h>
#include <stdlib.h>

#include <poll.h>
#include <time.h>
#include <signal.h>

#include <linux/sockios.h>

#include <endian.h>

#include "mrp_client.h"
#include "maap_client.h"
#include "avpdu.h"
#include "avb_log.h"
#include "msrp_domain.h"

#include <jdksavdecc.h>
#include <../kernel-module/snd-igb-avb/src/avb-config.h>

#include "utils.h"
#include "gptp_client.h"
#include "msrp_talker.h"
#include "avb_status.h"
#include "avtp.h"

#include "avdecc_adp.h"

extern int sendMsg(int sock, char* buffer, int buflen);

extern char DMAC[];

#define MCAST_BASE MAAP_LOCALLY_ADMINISTRATORED_BASE

extern uint64_t g_device_source_mac;
extern uint64_t g_device_talker_mac;

extern uint64_t ENTITY_ID;
extern uint64_t MODEL_ID;
extern uint64_t GRANDMASTER_ID;

extern struct avb_status g_status;
extern pthread_mutex_t g_status_lock;

extern volatile sig_atomic_t global_available_index;

extern char MAC[6];
extern int ifindex;

#define HEADER_OFFSET 14

void avdecc_adp_send_announce(int sock)
{
    uint8_t buffer[100];
    struct jdksavdecc_adpdu adpdu;
    memset(&adpdu, 0, sizeof(adpdu));

    adpdu.header.cd = 1;
    adpdu.header.subtype = JDKSAVDECC_SUBTYPE_ADP;
    adpdu.header.sv = 0;
    adpdu.header.version = 0;
    adpdu.header.message_type = JDKSAVDECC_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE;
    adpdu.header.valid_time = 10;
    adpdu.header.control_data_length = 56;
    jdksavdecc_eui64_init_from_uint64(&adpdu.header.entity_id, ENTITY_ID);

    jdksavdecc_eui64_init_from_uint64(&adpdu.entity_model_id, MODEL_ID);
    adpdu.entity_capabilities =
        JDKSAVDECC_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED |
        JDKSAVDECC_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED |
        JDKSAVDECC_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED |
        JDKSAVDECC_ADP_ENTITY_CAPABILITY_AEM_INTERFACE_INDEX_VALID;
    adpdu.talker_stream_sources = g_num_streams;
    adpdu.talker_capabilities =
        JDKSAVDECC_ADP_TALKER_CAPABILITY_IMPLEMENTED |
        JDKSAVDECC_ADP_TALKER_CAPABILITY_AUDIO_SOURCE;
    adpdu.listener_stream_sinks = g_num_streams;
    adpdu.listener_capabilities =
        JDKSAVDECC_ADP_LISTENER_CAPABILITY_IMPLEMENTED |
        JDKSAVDECC_ADP_LISTENER_CAPABILITY_AUDIO_SINK;
    adpdu.controller_capabilities = 0;
    /* la_avdecc requires available_index to strictly increase per ENTITY_AVAILABLE; only bump it here. */
    adpdu.available_index = global_available_index++;
    status_update_available_index(adpdu.available_index);
    jdksavdecc_eui64_init_from_uint64(&adpdu.gptp_grandmaster_id,
                                      gptp_get_grandmaster_id());
    adpdu.gptp_domain_number = gptp_get_domain_number();
    adpdu.reserved0 = 0;
    adpdu.identify_control_index = 0;
    adpdu.interface_index = 0;
    jdksavdecc_eui64_init_from_uint64(&adpdu.association_id, 0);
    adpdu.reserved1 = 0;

    int pos = 0;
    memcpy(&buffer[pos], DMAC, 6); pos += 6;
    memcpy(&buffer[pos], MAC, 6); pos += 6;
    buffer[pos++] = 0x22;
    buffer[pos++] = 0xf0;
    jdksavdecc_adpdu_write(&adpdu, buffer + pos, 0, sizeof(buffer) - pos);

    sendMsg(sock, (char *)buffer, pos + JDKSAVDECC_ADPDU_LEN);
}

void avdecc_adp_send_departing(int sock)
{
    uint8_t buffer[100];
    struct jdksavdecc_adpdu adpdu;
    memset(&adpdu, 0, sizeof(adpdu));

    adpdu.header.cd = 1;
    adpdu.header.subtype = JDKSAVDECC_SUBTYPE_ADP;
    adpdu.header.sv = 0;
    adpdu.header.version = 0;
    adpdu.header.message_type = JDKSAVDECC_ADP_MESSAGE_TYPE_ENTITY_DEPARTING;
    adpdu.header.valid_time = 0;
    adpdu.header.control_data_length = 56;
    jdksavdecc_eui64_init_from_uint64(&adpdu.header.entity_id, ENTITY_ID);

    jdksavdecc_eui64_init_from_uint64(&adpdu.entity_model_id, MODEL_ID);
    adpdu.entity_capabilities =
        JDKSAVDECC_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED |
        JDKSAVDECC_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED |
        JDKSAVDECC_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED |
        JDKSAVDECC_ADP_ENTITY_CAPABILITY_AEM_INTERFACE_INDEX_VALID;
    adpdu.talker_stream_sources = g_num_streams;
    adpdu.talker_capabilities =
        JDKSAVDECC_ADP_TALKER_CAPABILITY_IMPLEMENTED |
        JDKSAVDECC_ADP_TALKER_CAPABILITY_AUDIO_SOURCE;
    adpdu.listener_stream_sinks = g_num_streams;
    adpdu.listener_capabilities =
        JDKSAVDECC_ADP_LISTENER_CAPABILITY_IMPLEMENTED |
        JDKSAVDECC_ADP_LISTENER_CAPABILITY_AUDIO_SINK;
    adpdu.controller_capabilities = 0;
    adpdu.available_index = global_available_index;
    jdksavdecc_eui64_init_from_uint64(&adpdu.gptp_grandmaster_id,
                                      gptp_get_grandmaster_id());
    adpdu.gptp_domain_number = gptp_get_domain_number();
    adpdu.reserved0 = 0;
    adpdu.identify_control_index = 0;
    adpdu.interface_index = 0;
    jdksavdecc_eui64_init_from_uint64(&adpdu.association_id, 0);
    adpdu.reserved1 = 0;

    int pos = 0;
    memcpy(&buffer[pos], DMAC, 6); pos += 6;
    memcpy(&buffer[pos], MAC, 6); pos += 6;
    buffer[pos++] = 0x22;
    buffer[pos++] = 0xf0;
    jdksavdecc_adpdu_write(&adpdu, buffer + pos, 0, sizeof(buffer) - pos);

    sendMsg(sock, (char *)buffer, pos + JDKSAVDECC_ADPDU_LEN);
}

void avdecc_adp_note_controller(uint64_t cid)
{
    static uint64_t seen[8];
    int i, known = 0, n;

    pthread_mutex_lock(&g_status_lock);
    n = g_status.controllers_seen;
    for (i = 0; i < n && i < 8; i++) if (seen[i] == cid) { known = 1; break; }
    if (!known && n < 8) { seen[n] = cid; g_status.controllers_seen = n + 1; }
    g_status.last_controller = cid;
    pthread_mutex_unlock(&g_status_lock);

    if (!known)
        avb_log(L_AVDECC, LOG_INFO, "controller %016llx is talking to our entity",
                (unsigned long long)cid);
}

void avdecc_adp_update_last_adp(uint64_t now_ms)
{
    pthread_mutex_lock(&g_status_lock);
    g_status.last_adp_ms = now_ms;
    pthread_mutex_unlock(&g_status_lock);
}

void avdecc_adp_init(uint64_t entity_id, uint64_t model_id, uint64_t grandmaster_id)
{
    ENTITY_ID = entity_id;
    MODEL_ID = model_id;
    GRANDMASTER_ID = grandmaster_id;
}

void avdecc_adp_cleanup(void)
{
}
