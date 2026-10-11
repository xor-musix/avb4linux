/******************************************************************************
 *
 *   Copyright (c) 2019, Ralf Beck
 *   Copyright (c) 2026, mnemonic powerplay <mnemonic@xor-musix.de>
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
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

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <endian.h>

#include "avb_log.h"
#include "utils.h"
#include "stream_identity.h"
#include "avdecc_adp.h"
#include "avdecc_aecp.h"
#include "avdecc_peers.h"
#include "avdecc_probe.h"
#include "descriptor.h"

extern int ifindex;
extern char MAC[6];

extern struct jdksavdecc_descriptor_entity entity;
extern struct jdksavdecc_descriptor_configuration configuration;
extern uint16_t configuration_descriptors[16];
extern struct jdksavdecc_descriptor_audio_unit audio_unit;
extern uint32_t samplerates[6];
extern struct jdksavdecc_descriptor_stream stream_input[MAX_STREAMS];
extern struct jdksavdecc_descriptor_stream stream_output[MAX_STREAMS];
extern struct jdksavdecc_descriptor_avb_interface avb_interface;
extern struct jdksavdecc_descriptor_clock_source clock_source[NUM_CLOCK_SOURCES];
extern struct jdksavdecc_descriptor_clock_domain clock_domain;
extern struct jdksavdecc_descriptor_locale locale;

extern uint64_t STREAM_INPUT_FORMAT_0;
extern uint64_t STREAM_INPUT_FORMAT_1;
extern uint64_t STREAM_INPUT_FORMAT_2;
extern uint64_t STREAM_INPUT_FORMAT_3;
extern uint64_t STREAM_INPUT_FORMAT_4;
extern uint64_t STREAM_INPUT_FORMAT_5;
extern uint64_t STREAM_OUTPUT_FORMAT_0;
extern uint64_t STREAM_OUTPUT_FORMAT_1;
extern uint64_t STREAM_OUTPUT_FORMAT_2;
extern uint64_t STREAM_OUTPUT_FORMAT_3;
extern uint64_t STREAM_OUTPUT_FORMAT_4;
extern uint64_t STREAM_OUTPUT_FORMAT_5;

int sendMsg(int sock, char* buffer, int buflen)
{
    int sendlen;

    struct sockaddr_ll sadr_ll;
    sadr_ll.sll_family = AF_PACKET;
    sadr_ll.sll_ifindex = ifindex;
    sadr_ll.sll_halen = ETH_ALEN;
    sadr_ll.sll_addr[0] = buffer[0];
    sadr_ll.sll_addr[1] = buffer[1];
    sadr_ll.sll_addr[2] = buffer[2];
    sadr_ll.sll_addr[3] = buffer[3];
    sadr_ll.sll_addr[4] = buffer[4];
    sadr_ll.sll_addr[5] = buffer[5];

    sendlen = sendto(sock, buffer , buflen, 0, (const struct sockaddr*)&sadr_ll, sizeof(struct sockaddr_ll));
    if (sendlen < 0)
    {
       static uint64_t last_log;
       uint64_t now = get_time_in_ms();
       if (now - last_log > 1000) { last_log = now;
          avb_log(L_SYS, LOG_WARNING, "sendto failed (%s)", strerror(errno)); }
    }
    return sendlen;
}

extern uint32_t samplerate;

extern void avdecc_adp_note_controller(uint64_t controller_id);

extern int ifindex;
extern char MAC[6];
extern char DMAC[];
extern uint64_t ENTITY_ID;
extern uint64_t CONTROLLER_ID;
extern uint64_t g_device_source_mac;
extern unsigned global_available_index;

#define HEADER_OFFSET 14
#define MACLEN 6

void handle_entity_descriptor(int sock, char *buffer, int len)
{
    entity.available_index = global_available_index;
    jdksavdecc_common_control_header_set_control_data_length(328, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_entity_write(&entity, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_ENTITY_LEN + 0x2A);

    sendMsg(sock, buffer, 354);
}

void handle_configuration_descriptor(int sock, char *buffer, int len)
{
    jdksavdecc_common_control_header_set_control_data_length(122, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_configuration_write(&configuration, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_CONFIGURATION_LEN + 0x2A);
    memcpy(&buffer[0x74], configuration_descriptors, sizeof(configuration_descriptors));

    sendMsg(sock, buffer, 148);
}

void handle_audio_unit_descriptor(int sock, char *buffer, int len)
{
    jdksavdecc_common_control_header_set_control_data_length(184, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_audio_unit_write(&audio_unit, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_AUDIO_UNIT_LEN + 0x2A);
    memcpy(&buffer[0xba], samplerates, sizeof(samplerates));

    sendMsg(sock, buffer, 210);
}

void handle_no_such_descriptor(int sock, char *buffer, int len);

void handle_stream_input_descriptor(int sock, char *buffer, int len)
{
    uint16_t i = jdksavdecc_descriptor_stream_get_descriptor_index(buffer, 0x2A);

    if (i >= g_num_streams)
    {
        handle_no_such_descriptor(sock, buffer, len);
        return;
    }

    jdksavdecc_common_control_header_set_control_data_length(196, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_stream_write(&stream_input[i], buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_STREAM_LEN + 0x2A);
    memcpy(&buffer[0xae], &STREAM_INPUT_FORMAT_0, sizeof(STREAM_INPUT_FORMAT_0));
    memcpy(&buffer[0xae + 8], &STREAM_INPUT_FORMAT_1, sizeof(STREAM_INPUT_FORMAT_1));
    memcpy(&buffer[0xae + 16], &STREAM_INPUT_FORMAT_2, sizeof(STREAM_INPUT_FORMAT_2));
    memcpy(&buffer[0xae + 24], &STREAM_INPUT_FORMAT_3, sizeof(STREAM_INPUT_FORMAT_3));
    memcpy(&buffer[0xae + 32], &STREAM_INPUT_FORMAT_4, sizeof(STREAM_INPUT_FORMAT_4));
    memcpy(&buffer[0xae + 40], &STREAM_INPUT_FORMAT_5, sizeof(STREAM_INPUT_FORMAT_5));

    sendMsg(sock, buffer, 222);
}

void handle_stream_output_descriptor(int sock, char *buffer, int len)
{
    uint16_t i = jdksavdecc_descriptor_stream_get_descriptor_index(buffer, 0x2A);

    if (i >= g_num_streams)
    {
        handle_no_such_descriptor(sock, buffer, len);
        return;
    }

    jdksavdecc_common_control_header_set_control_data_length(196, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_stream_write(&stream_output[i], buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_STREAM_LEN + 0x2A);
    memcpy(&buffer[0xae], &STREAM_OUTPUT_FORMAT_0, sizeof(STREAM_OUTPUT_FORMAT_0));
    memcpy(&buffer[0xae + 8], &STREAM_OUTPUT_FORMAT_1, sizeof(STREAM_OUTPUT_FORMAT_1));
    memcpy(&buffer[0xae + 16], &STREAM_OUTPUT_FORMAT_2, sizeof(STREAM_OUTPUT_FORMAT_2));
    memcpy(&buffer[0xae + 24], &STREAM_OUTPUT_FORMAT_3, sizeof(STREAM_OUTPUT_FORMAT_3));
    memcpy(&buffer[0xae + 32], &STREAM_OUTPUT_FORMAT_4, sizeof(STREAM_OUTPUT_FORMAT_4));
    memcpy(&buffer[0xae + 40], &STREAM_OUTPUT_FORMAT_5, sizeof(STREAM_OUTPUT_FORMAT_5));

    sendMsg(sock, buffer, 222);
}

void handle_avb_interface_descriptor(int sock, char *buffer, int len)
{
    jdksavdecc_common_control_header_set_control_data_length(114, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_avb_interface_write(&avb_interface, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_AVB_INTERFACE_LEN + 0x2A);

    sendMsg(sock, buffer, 140);
}


void handle_clock_source_descriptor(int sock, char *buffer, int len)
{
    uint16_t idx = jdksavdecc_descriptor_clock_source_get_descriptor_index(buffer, 0x2A);

    if (idx >= NUM_CLOCK_SOURCES)
    {
        handle_no_such_descriptor(sock, buffer, len);
        return;
    }

    avb_log(L_AECP, LOG_DEBUG, "handle_clock_source_descriptor: index=%u type=%d%s",
            idx, clock_source[idx].clock_source_type,
            (idx == descriptor_get_clock_source_index()) ? " (active)" : "");

    jdksavdecc_common_control_header_set_control_data_length(102, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_clock_source_write(&clock_source[idx], buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_CLOCK_SOURCE_LEN + 0x2A);

    sendMsg(sock, buffer, 128);
}

void handle_locale_descriptor(int sock, char *buffer, int len)
{
    jdksavdecc_common_control_header_set_control_data_length(88, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_locale_write(&locale, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_LOCALE_LEN + 0x2A);

    sendMsg(sock, buffer, 114);
}

void handle_clock_domain_descriptor(int sock, char *buffer, int len)
{
    jdksavdecc_descriptor_clock_domain_write(&clock_domain, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN_LEN + 0x2A);

    jdksavdecc_uint16_set(CLOCK_SOURCE_INDEX_STREAM,   buffer, 0x2A + JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN_OFFSET_CLOCK_SOURCES);
    jdksavdecc_uint16_set(CLOCK_SOURCE_INDEX_INTERNAL, buffer, 0x2A + JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN_OFFSET_CLOCK_SOURCES + 2);

    jdksavdecc_common_control_header_set_control_data_length(96, buffer, HEADER_OFFSET);

    sendMsg(sock, buffer, 122);
}

void handle_strings_descriptor(int sock, char *buffer, int len)
{
    struct jdksavdecc_descriptor_strings strings;

    memset(&strings, 0, sizeof(strings));

    strings.descriptor_type = JDKSAVDECC_DESCRIPTOR_STRINGS;
    strings.descriptor_index = 0;
    jdksavdecc_string_set_from_cstr(&strings.string_0, "Drumfix");
    jdksavdecc_string_set_from_cstr(&strings.string_1, "Linux AVB");
    jdksavdecc_string_set_from_cstr(&strings.string_2, "Internal");
    jdksavdecc_string_set_from_cstr(&strings.string_3, "");
    jdksavdecc_string_set_from_cstr(&strings.string_4, "");
    jdksavdecc_string_set_from_cstr(&strings.string_5, "");
    jdksavdecc_string_set_from_cstr(&strings.string_6, "");

    jdksavdecc_common_control_header_set_control_data_length(468, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_strings_write(&strings, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_STRINGS_LEN + 0x2A);

    sendMsg(sock, buffer, 494);
}

void handle_stream_port_input_descriptor(int sock, char *buffer, int len)
{
    struct jdksavdecc_descriptor_stream_port spi;

    memset(&spi, 0, sizeof(spi));

    spi.descriptor_type = JDKSAVDECC_DESCRIPTOR_STREAM_PORT_INPUT;
    spi.descriptor_index = jdksavdecc_descriptor_stream_get_descriptor_index(buffer, 0x2A);

    if (spi.descriptor_index >= g_num_streams)
    {
        handle_no_such_descriptor(sock, buffer, len);
        return;
    }
    spi.clock_domain_index = 0;
    spi.port_flags = 0;
    spi.number_of_controls = 0;
    spi.base_control = 0;
    spi.number_of_clusters = 8;
    spi.base_cluster = STREAM_PORT_INPUT_BASE_CLUSTER + 8 * spi.descriptor_index;
    spi.number_of_maps = 1;
    spi.base_map = STREAM_PORT_INPUT_BASE_MAP + spi.descriptor_index;

    jdksavdecc_common_control_header_set_control_data_length(36, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_stream_port_write(&spi, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_STREAM_PORT_LEN + 0x2A);

    sendMsg(sock, buffer, 64);
}

void handle_stream_port_output_descriptor(int sock, char *buffer, int len)
{
    struct jdksavdecc_descriptor_stream_port spo;

    memset(&spo, 0, sizeof(spo));

    spo.descriptor_type = JDKSAVDECC_DESCRIPTOR_STREAM_PORT_OUTPUT;
    spo.descriptor_index = jdksavdecc_descriptor_stream_get_descriptor_index(buffer, 0x2A);

    if (spo.descriptor_index >= g_num_streams)
    {
        handle_no_such_descriptor(sock, buffer, len);
        return;
    }
    spo.clock_domain_index = 0;
    spo.port_flags = JDKSAVDECC_DESCRIPTOR_STREAM_FLAG_CLOCK_SYNC_SOURCE;
    spo.number_of_controls = 0;
    spo.base_control = 0;
    spo.number_of_clusters = 8;
    spo.base_cluster = STREAM_PORT_OUTPUT_BASE_CLUSTER + 8 * spo.descriptor_index;
    spo.number_of_maps = 1;
    spo.base_map = STREAM_PORT_OUTPUT_BASE_MAP + spo.descriptor_index;

    jdksavdecc_common_control_header_set_control_data_length(36, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_stream_port_write(&spo, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_STREAM_PORT_LEN + 0x2A);

    sendMsg(sock, buffer, 64);
}

void handle_audio_cluster_descriptor(int sock, char *buffer, int len)
{
    struct jdksavdecc_descriptor_audio_cluster ac;

    memset(&ac, 0, sizeof(ac));

    ac.descriptor_type = JDKSAVDECC_DESCRIPTOR_AUDIO_CLUSTER;
    ac.descriptor_index = jdksavdecc_descriptor_stream_get_descriptor_index(buffer, 0x2A);

    if (ac.descriptor_index >= 2 * 8 * g_num_streams)
    {
        handle_no_such_descriptor(sock, buffer, len);
        return;
    }
    struct jdksavdecc_string object_name;
    jdksavdecc_string_set_from_cstr(&ac.object_name, "");
    ac.localized_description = -1;
    ac.signal_type = -1;
    ac.signal_index = 0;
    ac.signal_output = 0;
    ac.path_latency = 0;
    ac.block_latency = 0;
    ac.channel_count = 1;
    ac.format = JDKSAVDECC_AUDIO_CLUSTER_FORMAT_MBLA;

    jdksavdecc_common_control_header_set_control_data_length(103, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_audio_cluster_write(&ac, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_AUDIO_CLUSTER_LEN + 0x2A);

    sendMsg(sock, buffer, 129);
}

void handle_audio_map_descriptor(int sock, char *buffer, int len)
{
    struct jdksavdecc_descriptor_audio_map am;
    struct jdksavdecc_audio_mapping* amap = (struct jdksavdecc_audio_mapping*)(buffer + 50);
    uint16_t stream_index;
    int i;

    memset(&am, 0, sizeof(am));

    am.descriptor_type = JDKSAVDECC_DESCRIPTOR_AUDIO_MAP;
    am.descriptor_index = jdksavdecc_descriptor_stream_get_descriptor_index(buffer, 0x2A);

    if (am.descriptor_index >= 2 * g_num_streams)
    {
        handle_no_such_descriptor(sock, buffer, len);
        return;
    }

    /* mapping_stream_index is per direction: output map BASE_MAP+k is STREAM_OUTPUT k, not k+N. */
    stream_index = am.descriptor_index;
    if (stream_index >= STREAM_PORT_OUTPUT_BASE_MAP)
        stream_index -= STREAM_PORT_OUTPUT_BASE_MAP;

    am.mappings_offset = 8;
    am.number_of_mappings = 8;

    jdksavdecc_common_control_header_set_control_data_length(88, buffer, HEADER_OFFSET);
    jdksavdecc_descriptor_audio_map_write(&am, buffer, 0x2A, JDKSAVDECC_DESCRIPTOR_AUDIO_MAP_LEN + 0x2A);

    for (i=0; i<8; i++)
    {
        amap[i].mapping_stream_index = htons(stream_index);
        amap[i].mapping_stream_channel = htons(i);
        amap[i].mapping_cluster_offset = htons(i);
        amap[i].mapping_cluster_channel = 0;
    }

    sendMsg(sock, buffer, 114);
}

void handle_no_such_descriptor(int sock, char *buffer, int len)
{
    jdksavdecc_common_control_header_set_status(JDKSAVDECC_AEM_STATUS_NO_SUCH_DESCRIPTOR, buffer, HEADER_OFFSET);
    sendMsg(sock, buffer, len);
}

void handle_aem_command_read_descriptor(int sock, char *buffer, int len)
{
    switch (jdksavdecc_descriptor_entity_get_descriptor_type(buffer, 0x2A))
    {
       case JDKSAVDECC_DESCRIPTOR_ENTITY:
       {
          handle_entity_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_CONFIGURATION:
       {
          handle_configuration_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_AUDIO_UNIT:
       {
          handle_audio_unit_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_STREAM_INPUT:
       {
          handle_stream_input_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_STREAM_OUTPUT:
       {
          handle_stream_output_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_AVB_INTERFACE:
       {
          handle_avb_interface_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_CLOCK_SOURCE:
       {
          handle_clock_source_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_LOCALE:
       {
          handle_locale_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_STRINGS:
       {
          handle_strings_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_STREAM_PORT_INPUT:
       {
          handle_stream_port_input_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_STREAM_PORT_OUTPUT:
       {
          handle_stream_port_output_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN:
       {
          handle_clock_domain_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_AUDIO_CLUSTER:
       {
          handle_audio_cluster_descriptor(sock, buffer, len);
          break;
       }
       case JDKSAVDECC_DESCRIPTOR_AUDIO_MAP:
       {
          handle_audio_map_descriptor(sock, buffer, len);
          break;
       }
       default:
          handle_no_such_descriptor(sock, buffer, len);
          break;
    }
}

void handle_unimplemented_aem_command(int sock, char *buffer, int len)
{
    jdksavdecc_common_control_header_set_status(JDKSAVDECC_AEM_STATUS_NOT_IMPLEMENTED, buffer, HEADER_OFFSET);

    sendMsg(sock, buffer, len);
}

void handle_get_stream_format(int sock, char *buffer, int len)
{
    uint16_t dtype = jdksavdecc_uint16_get(buffer,
        HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT_RESPONSE_OFFSET_DESCRIPTOR_TYPE);
    uint16_t didx  = jdksavdecc_uint16_get(buffer,
        HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT_RESPONSE_OFFSET_DESCRIPTOR_INDEX);

    struct jdksavdecc_eui64 fmt;
    if (didx >= g_num_streams)
    {
        handle_no_such_descriptor(sock, buffer, len);
        return;
    }
    if (dtype == JDKSAVDECC_DESCRIPTOR_STREAM_OUTPUT)
        fmt = stream_output[didx].current_format;
    else
        fmt = stream_input[didx].current_format;

    jdksavdecc_eui64_set(fmt, buffer,
        HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT_RESPONSE_OFFSET_STREAM_FORMAT);

    jdksavdecc_common_control_header_set_control_data_length(
        JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT_RESPONSE_LEN - JDKSAVDECC_COMMON_CONTROL_HEADER_LEN,
        buffer, HEADER_OFFSET);

    sendMsg(sock, buffer, HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT_RESPONSE_LEN);
}

void handle_set_stream_format(int sock, char *buffer, int len)
{
    uint16_t dtype = jdksavdecc_uint16_get(buffer,
        HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_SET_STREAM_FORMAT_COMMAND_OFFSET_DESCRIPTOR_TYPE);
    uint16_t didx  = jdksavdecc_uint16_get(buffer,
        HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_SET_STREAM_FORMAT_COMMAND_OFFSET_DESCRIPTOR_INDEX);
    struct jdksavdecc_eui64 fmt = jdksavdecc_eui64_get(buffer,
        HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_SET_STREAM_FORMAT_COMMAND_OFFSET_STREAM_FORMAT);

    if (didx >= g_num_streams)
    {
        handle_no_such_descriptor(sock, buffer, len);
        return;
    }
    if (dtype == JDKSAVDECC_DESCRIPTOR_STREAM_OUTPUT)
        stream_output[didx].current_format = fmt;
    else
        stream_input[didx].current_format = fmt;

    jdksavdecc_eui64_set(fmt, buffer,
        HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_SET_STREAM_FORMAT_RESPONSE_OFFSET_STREAM_FORMAT);

    jdksavdecc_common_control_header_set_control_data_length(
        JDKSAVDECC_AEM_COMMAND_SET_STREAM_FORMAT_RESPONSE_LEN - JDKSAVDECC_COMMON_CONTROL_HEADER_LEN,
        buffer, HEADER_OFFSET);

    sendMsg(sock, buffer, HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_SET_STREAM_FORMAT_RESPONSE_LEN);
}

void handle_get_sampling_rate(int sock, char *buffer, int len)
{
    jdksavdecc_uint32_set(samplerate, buffer,
        HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_GET_SAMPLING_RATE_RESPONSE_OFFSET_SAMPLING_RATE);

    jdksavdecc_common_control_header_set_control_data_length(
        JDKSAVDECC_AEM_COMMAND_GET_SAMPLING_RATE_RESPONSE_LEN - JDKSAVDECC_COMMON_CONTROL_HEADER_LEN,
        buffer, HEADER_OFFSET);

    sendMsg(sock, buffer, HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_GET_SAMPLING_RATE_RESPONSE_LEN);
}

void handle_get_clock_source(int sock, char *buffer, int len)
{
    uint16_t idx = descriptor_get_clock_source_index();

    jdksavdecc_uint16_set(idx, buffer,
        HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_GET_CLOCK_SOURCE_RESPONSE_OFFSET_CLOCK_SOURCE_INDEX);

    jdksavdecc_common_control_header_set_control_data_length(
        JDKSAVDECC_AEM_COMMAND_GET_CLOCK_SOURCE_RESPONSE_LEN - JDKSAVDECC_COMMON_CONTROL_HEADER_LEN,
        buffer, HEADER_OFFSET);

    sendMsg(sock, buffer, HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_GET_CLOCK_SOURCE_RESPONSE_LEN);
}

void handle_aem_command(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 cid = jdksavdecc_aecpdu_aem_get_controller_entity_id(buffer, HEADER_OFFSET);
    avdecc_adp_note_controller(jdksavdecc_eui64_convert_to_uint64(&cid));

    memcpy(buffer, &buffer[MACLEN], MACLEN);
    memcpy(&buffer[MACLEN], MAC, MACLEN);
    jdksavdecc_common_control_header_set_control_data(JDKSAVDECC_AECP_MESSAGE_TYPE_AEM_RESPONSE, buffer, HEADER_OFFSET);
    jdksavdecc_common_control_header_set_status(JDKSAVDECC_AEM_STATUS_SUCCESS, buffer, HEADER_OFFSET);

    switch (jdksavdecc_aecpdu_aem_get_command_type(buffer, HEADER_OFFSET))
    {
       case JDKSAVDECC_AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION:
       case JDKSAVDECC_AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION:
       {
         sendMsg(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR:
       {
         handle_aem_command_read_descriptor(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_AEM_COMMAND_SET_STREAM_FORMAT:
       {
         handle_set_stream_format(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT:
       {
         handle_get_stream_format(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_AEM_COMMAND_GET_SAMPLING_RATE:
       {
         handle_get_sampling_rate(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_AEM_COMMAND_GET_CLOCK_SOURCE:
       {
         handle_get_clock_source(sock, buffer, len);
         break;
       }
       default:
       {
         handle_unimplemented_aem_command(sock, buffer, len);
         break;
       }
    }
}

void avdecc_aecp_init(void)
{
    avb_log(L_AECP, LOG_DEBUG, "avdecc_aecp_init: initializing AECP module");
}

void avdecc_aecp_handle_command(int sock, char *buffer, int len)
{
    struct jdksavdecc_eui64 target_id = jdksavdecc_common_control_header_get_stream_id(buffer, HEADER_OFFSET);
    uint64_t target_id_a = jdksavdecc_eui64_convert_to_uint64(&target_id);

    switch (jdksavdecc_common_control_header_get_control_data(buffer, HEADER_OFFSET))
    {
       case JDKSAVDECC_AECP_MESSAGE_TYPE_AEM_COMMAND:
       {
         if (target_id_a != ENTITY_ID)
         {
            return;
         }
         handle_aem_command(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_AECP_MESSAGE_TYPE_AEM_RESPONSE:
       {
         avdecc_peers_handle_aem_response(buffer, len);
         avdecc_probe_handle_aem_response(sock, buffer, len);
         break;
       }
       case JDKSAVDECC_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND:
       {
         if (target_id_a != CONTROLLER_ID)
         {
            return;
         }
         memcpy(buffer, &buffer[MACLEN], MACLEN);
         memcpy(&buffer[MACLEN], MAC, MACLEN);
         jdksavdecc_common_control_header_set_control_data
           (JDKSAVDECC_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE, buffer, HEADER_OFFSET);
         jdksavdecc_common_control_header_set_status
           (JDKSAVDECC_AEM_STATUS_SUCCESS, buffer, HEADER_OFFSET);
         sendMsg(sock, buffer, len);
         break;
       }
       default:
         break;
    }
}

void avdecc_aecp_set_samplerate(uint32_t rate)
{
    samplerate = rate;
}

void avdecc_aecp_send_set_sampling_rate(int sock, uint32_t rate)
{
    char buffer[128];
    struct jdksavdecc_aem_command_set_sampling_rate aem_cmd;

    avb_log(L_AVDECC, LOG_INFO, "setting remote samplerate to %d", rate);

    aem_cmd.aem_header.aecpdu_header.header.cd = 1;
    aem_cmd.aem_header.aecpdu_header.header.subtype = JDKSAVDECC_SUBTYPE_AECP;
    aem_cmd.aem_header.aecpdu_header.header.sv = 0;
    aem_cmd.aem_header.aecpdu_header.header.version = 0;
    aem_cmd.aem_header.aecpdu_header.header.message_type = JDKSAVDECC_AECP_MESSAGE_TYPE_AEM_COMMAND;
    aem_cmd.aem_header.aecpdu_header.header.status = JDKSAVDECC_AEM_STATUS_SUCCESS;
    aem_cmd.aem_header.aecpdu_header.header.control_data_length =
        JDKSAVDECC_AEM_COMMAND_SET_SAMPLING_RATE_COMMAND_LEN - JDKSAVDECC_COMMON_CONTROL_HEADER_LEN;
    jdksavdecc_eui64_init_from_uint64(&aem_cmd.aem_header.aecpdu_header.header.target_entity_id, mac_to_entity_id(g_device_source_mac));
    jdksavdecc_eui64_init_from_uint64(&aem_cmd.aem_header.aecpdu_header.controller_entity_id, CONTROLLER_ID);
    aem_cmd.aem_header.aecpdu_header.sequence_id = 1;
    aem_cmd.aem_header.command_type = JDKSAVDECC_AEM_COMMAND_SET_SAMPLING_RATE;
    aem_cmd.descriptor_type = JDKSAVDECC_DESCRIPTOR_AUDIO_UNIT;
    aem_cmd.descriptor_index = 0;
    aem_cmd.sampling_rate = rate;

    memcpy(buffer, DMAC, MACLEN);
    memcpy(&buffer[MACLEN], MAC, MACLEN);
    buffer[12] = 0x22; buffer[13] = 0xf0;
    jdksavdecc_aem_command_set_sampling_rate_write(&aem_cmd, buffer, HEADER_OFFSET, sizeof(buffer));

    sendMsg(sock, buffer, HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_SET_SAMPLING_RATE_COMMAND_LEN);
}

uint32_t avdecc_aecp_get_samplerate(void)
{
    return samplerate;
}

void avdecc_aecp_init_descriptors(void)
{
    avb_log(L_AECP, LOG_DEBUG, "avdecc_aecp_init_descriptors: descriptors initialized");
}
