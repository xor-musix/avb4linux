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

#include <stdio.h>
#include <string.h>
#include <endian.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "avb_log.h"
#include "utils.h"
#include "descriptor.h"
#include "gptp_client.h"
#include "avb_status.h"
#include "stream_identity.h"
#include "../include/stream_id.h"

static char g_entity_hostname[64] = "";

struct jdksavdecc_descriptor_entity entity;

struct jdksavdecc_descriptor_configuration configuration;
uint16_t configuration_descriptors[16];

struct jdksavdecc_descriptor_audio_unit audio_unit;

uint32_t samplerates[6];

struct jdksavdecc_descriptor_stream stream_input[MAX_STREAMS];
struct jdksavdecc_descriptor_stream stream_output[MAX_STREAMS];

struct jdksavdecc_descriptor_avb_interface avb_interface;

struct jdksavdecc_descriptor_clock_source clock_source[NUM_CLOCK_SOURCES];
struct jdksavdecc_descriptor_clock_domain clock_domain;

struct jdksavdecc_descriptor_locale locale;

extern uint32_t samplerate;

uint64_t STREAM_INPUT_FORMAT_0 = 0x000800600801A000;
uint64_t STREAM_INPUT_FORMAT_1 = 0x000800600802A000;
uint64_t STREAM_INPUT_FORMAT_2 = 0x000800600803A000;
uint64_t STREAM_INPUT_FORMAT_3 = 0x000800600804A000;
uint64_t STREAM_INPUT_FORMAT_4 = 0x000800600805A000;
uint64_t STREAM_INPUT_FORMAT_5 = 0x000800600806A000;

uint64_t STREAM_INPUT_FORMAT_1_REVERSE =  0x00A0020840000800;

uint64_t STREAM_OUTPUT_FORMAT_0 = 0x000800600801A000;
uint64_t STREAM_OUTPUT_FORMAT_1 = 0x000800600802A000;
uint64_t STREAM_OUTPUT_FORMAT_2 = 0x000800600803A000;
uint64_t STREAM_OUTPUT_FORMAT_3 = 0x000800600804A000;
uint64_t STREAM_OUTPUT_FORMAT_4 = 0x000800600805A000;
uint64_t STREAM_OUTPUT_FORMAT_5 = 0x000800600806A000;

uint64_t STREAM_OUTPUT_FORMAT_1_REVERSE =  0x00A0020840000800;

extern int sendMsg(int sock, char* buffer, int buflen);

extern uint64_t ENTITY_ID;
extern uint64_t MODEL_ID;
extern uint64_t GRANDMASTER_ID;
extern char MAC[6];

void descriptor_set_entity_name(const char *hostname)
{
    strncpy(g_entity_hostname, hostname, sizeof(g_entity_hostname) - 1);
    g_entity_hostname[sizeof(g_entity_hostname) - 1] = '\0';
}

const char *descriptor_get_entity_name(void)
{
    static char name[64];
    if (g_entity_hostname[0] != '\0') {
        snprintf(name, sizeof(name), "%s AVB", g_entity_hostname);
    } else {
        snprintf(name, sizeof(name), "AVB");
    }
    return name;
}

void descriptor_fill_entity(void)
{
    char entity_name[64];

    memset(&entity, 0, sizeof(entity));

    entity.descriptor_type = JDKSAVDECC_DESCRIPTOR_ENTITY;
    entity.descriptor_index = 0;
    jdksavdecc_eui64_init_from_uint64(&entity.entity_id, ENTITY_ID);
    jdksavdecc_eui64_init_from_uint64(&entity.entity_model_id, MODEL_ID);

    entity.entity_capabilities =
       JDKSAVDECC_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED |
       JDKSAVDECC_ADP_ENTITY_CAPABILITY_CLASS_A_SUPPORTED |
       JDKSAVDECC_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED |
       JDKSAVDECC_ADP_ENTITY_CAPABILITY_AEM_INTERFACE_INDEX_VALID;
    entity.talker_stream_sources = g_num_streams;
    entity.talker_capabilities =
       JDKSAVDECC_ADP_TALKER_CAPABILITY_IMPLEMENTED |
       JDKSAVDECC_ADP_TALKER_CAPABILITY_AUDIO_SOURCE;
    entity.listener_stream_sinks = g_num_streams;
    entity.listener_capabilities =
       JDKSAVDECC_ADP_LISTENER_CAPABILITY_IMPLEMENTED |
       JDKSAVDECC_ADP_LISTENER_CAPABILITY_AUDIO_SINK;
    entity.controller_capabilities = 0;
    entity.available_index = 0;
    jdksavdecc_eui64_init_from_uint64(&entity.association_id, 0);

    if (g_entity_hostname[0] != '\0') {
        snprintf(entity_name, sizeof(entity_name), "%s AVB", g_entity_hostname);
        jdksavdecc_string_set_from_cstr(&entity.entity_name, entity_name);
    } else {
        jdksavdecc_string_set_from_cstr(&entity.entity_name, "AVB");
    }

    entity.vendor_name_string = 0;
    entity.model_name_string = 1;
    jdksavdecc_string_set_from_cstr(&entity.firmware_version, "1.0.0");
    jdksavdecc_string_set_from_cstr(&entity.group_name, "");
    jdksavdecc_string_set_from_cstr(&entity.serial_number, "1");
    entity.configurations_count = 1;
    entity.current_configuration = 0;
}

void descriptor_fill_configuration(void)
{
    memset(&configuration, 0, sizeof(configuration));

    configuration.descriptor_type = JDKSAVDECC_DESCRIPTOR_CONFIGURATION;
    configuration.descriptor_index = 0;
    jdksavdecc_string_set_from_cstr(&configuration.object_name, "");
    configuration.localized_description = 0xffff;
    configuration.descriptor_counts_count = 8;
    configuration.descriptor_counts_offset  = 74;

    configuration_descriptors[0] = htons(JDKSAVDECC_DESCRIPTOR_AUDIO_UNIT);
    configuration_descriptors[1] = htons(1);
    configuration_descriptors[2] = htons(JDKSAVDECC_DESCRIPTOR_STREAM_INPUT);
    configuration_descriptors[3] = htons(g_num_streams);
    configuration_descriptors[4] = htons(JDKSAVDECC_DESCRIPTOR_STREAM_OUTPUT);
    configuration_descriptors[5] = htons(g_num_streams);
    configuration_descriptors[6] = htons(JDKSAVDECC_DESCRIPTOR_AVB_INTERFACE);
    configuration_descriptors[7] = htons(1);
    configuration_descriptors[8] = htons(JDKSAVDECC_DESCRIPTOR_CLOCK_SOURCE);
    configuration_descriptors[9] = htons(1);
    configuration_descriptors[10] = htons(JDKSAVDECC_DESCRIPTOR_LOCALE);
    configuration_descriptors[11] = htons(1);
    configuration_descriptors[12] = htons(JDKSAVDECC_DESCRIPTOR_STRINGS);
    configuration_descriptors[13] = htons(1);
    configuration_descriptors[14] = htons(JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN);
    configuration_descriptors[15] = htons(1);
}

void descriptor_fill_audio_unit(void)
{
    audio_unit.descriptor_type = JDKSAVDECC_DESCRIPTOR_AUDIO_UNIT;
    audio_unit.descriptor_index = 0;
    jdksavdecc_string_set_from_cstr(&audio_unit.object_name, "");
    audio_unit.localized_description = 1;
    audio_unit.clock_domain_index = 0;
    audio_unit.number_of_stream_input_ports = g_num_streams;
    audio_unit.base_stream_input_port = 0;
    audio_unit.number_of_stream_output_ports = g_num_streams;
    audio_unit.base_stream_output_port = 0;
    audio_unit.number_of_external_input_ports = 0;
    audio_unit.base_external_input_port = 0;
    audio_unit.number_of_external_output_ports = 0;
    audio_unit.base_external_output_port = 0;
    audio_unit.number_of_internal_input_ports = 0;
    audio_unit.base_internal_input_port = 0;
    audio_unit.number_of_internal_output_ports = 0;
    audio_unit.base_internal_output_port = 0;
    audio_unit.number_of_controls = 0;
    audio_unit.base_control = 0;
    audio_unit.number_of_signal_selectors = 0;
    audio_unit.base_signal_selector = 0;
    audio_unit.number_of_mixers = 0;
    audio_unit.base_mixer = 0;
    audio_unit.number_of_matrices = 0;
    audio_unit.base_matrix = 0;
    audio_unit.number_of_splitters = 0;
    audio_unit.base_splitter = 0;
    audio_unit.number_of_combiners = 0;
    audio_unit.base_combiner = 0;
    audio_unit.number_of_demultiplexers = 0;
    audio_unit.base_demultiplexer = 0;
    audio_unit.number_of_multiplexers = 0;
    audio_unit.base_multiplexer = 0;
    audio_unit.number_of_transcoders = 0;
    audio_unit.base_transcoder = 0;
    audio_unit.number_of_control_blocks = 0;
    audio_unit.base_control_block = 0;
    audio_unit.current_sampling_rate = samplerate;
    audio_unit.sampling_rates_offset = 144;
    audio_unit.sampling_rates_count = 6;

    samplerates[0] = htonl(44100);
    samplerates[1] = htonl(48000);
    samplerates[2] = htonl(88200);
    samplerates[3] = htonl(96000);
    samplerates[4] = htonl(176400);
    samplerates[5] = htonl(192000);
}

void descriptor_fill_streams(void)
{
    int i;
    char name[64];

    uint64_t stream_format = 0;

    switch (samplerate)
    {
      case 44100:
        stream_format = be64toh(STREAM_INPUT_FORMAT_0);
        break;
      case 48000:
        stream_format = be64toh(STREAM_INPUT_FORMAT_1);
        break;
      case 88200:
        stream_format = be64toh(STREAM_INPUT_FORMAT_2);
        break;
      case 96000:
        stream_format = be64toh(STREAM_INPUT_FORMAT_3);
        break;
      case 176400:
        stream_format = be64toh(STREAM_INPUT_FORMAT_4);
        break;
      case 192000:
        stream_format = be64toh(STREAM_INPUT_FORMAT_5);
        break;
    }

    for (i = 0; i < MAX_STREAMS; i++)
    {
       sprintf(name, "%i", i);
       stream_input[i].descriptor_type = JDKSAVDECC_DESCRIPTOR_STREAM_INPUT;
       stream_input[i].descriptor_index = i;
       jdksavdecc_string_set_from_cstr(&stream_input[i].object_name, name);
       stream_input[i].localized_description = -1;
       stream_input[i].clock_domain_index = 0;
       stream_input[i].stream_flags =
         JDKSAVDECC_DESCRIPTOR_STREAM_FLAG_CLOCK_SYNC_SOURCE |
         JDKSAVDECC_DESCRIPTOR_STREAM_FLAG_CLASS_A;
       jdksavdecc_eui64_init_from_uint64(&stream_input[i].current_format, stream_format);
       stream_input[i].formats_offset= 132;
       stream_input[i].number_of_formats = 6;
       jdksavdecc_eui64_init_from_uint64(&stream_input[i].backup_talker_entity_id_0, 0);
       stream_input[i].backup_talker_unique_id_0  = 0;
       jdksavdecc_eui64_init_from_uint64(&stream_input[i].backup_talker_entity_id_1, 0);
       stream_input[i].backup_talker_unique_id_1 = 0;
       jdksavdecc_eui64_init_from_uint64(&stream_input[i].backup_talker_entity_id_2, 0);
       stream_input[i].backup_talker_unique_id_2 = 0;
       jdksavdecc_eui64_init_from_uint64(&stream_input[i].backedup_talker_entity_id, 0);
       stream_input[i].backedup_talker_unique = 0;
       stream_input[i].avb_interface_index = 0;
       stream_input[i].buffer_length = 8;

       stream_output[i].descriptor_type = JDKSAVDECC_DESCRIPTOR_STREAM_OUTPUT;
       stream_output[i].descriptor_index = i;
       jdksavdecc_string_set_from_cstr(&stream_output[i].object_name, name);
       stream_output[i].localized_description = -1;
       stream_output[i].clock_domain_index = 0;
       stream_output[i].stream_flags = JDKSAVDECC_DESCRIPTOR_STREAM_FLAG_CLASS_A;
       jdksavdecc_eui64_init_from_uint64(&stream_output[i].current_format, stream_format);
       stream_output[i].formats_offset= 132;
       stream_output[i].number_of_formats = 6;
       jdksavdecc_eui64_init_from_uint64(&stream_output[i].backup_talker_entity_id_0, 0);
       stream_output[i].backup_talker_unique_id_0  = 0;
       jdksavdecc_eui64_init_from_uint64(&stream_output[i].backup_talker_entity_id_1, 0);
       stream_output[i].backup_talker_unique_id_1 = 0;
       jdksavdecc_eui64_init_from_uint64(&stream_output[i].backup_talker_entity_id_2, 0);
       stream_output[i].backup_talker_unique_id_2 = 0;
       jdksavdecc_eui64_init_from_uint64(&stream_output[i].backedup_talker_entity_id, 0);
       stream_output[i].backedup_talker_unique = 0;
       stream_output[i].avb_interface_index = 0;
       stream_output[i].buffer_length = 8;
    }
}

void descriptor_fill_avb_interface(void)
{
    memset(&avb_interface, 0, sizeof(avb_interface));

    avb_interface.descriptor_type = JDKSAVDECC_DESCRIPTOR_AVB_INTERFACE;
    avb_interface.descriptor_index = 0;
    jdksavdecc_string_set_from_cstr(&avb_interface.object_name, "");
    avb_interface.localized_description = 0;
    memcpy(&avb_interface.mac_address.value, MAC, 6);
    avb_interface.interface_flags = htons
                  (JDKSAVDECC_AVB_INTERFACE_FLAG_GPTP_GRANDMASTER_SUPPORTED |
                   JDKSAVDECC_AVB_INTERFACE_FLAG_GPTP_SUPPORTED |
                   JDKSAVDECC_AVB_INTERFACE_FLAG_SRP_SUPPORTED);
    jdksavdecc_eui64_init_from_uint64(&avb_interface.clock_identity, GRANDMASTER_ID);
    avb_interface.priority1 = 240;
    avb_interface.clock_class = 248;
    avb_interface.offset_scaled_log_variance = 17258;
    avb_interface.clock_accuracy = 248;
    avb_interface.priority2 = 238;
    avb_interface.domain_number = 0;
    avb_interface.log_sync_interval = -2;
    avb_interface.log_announce_interval = 0;
    avb_interface.log_pdelay_interval = 0;
    avb_interface.port_number = 1;
}

void descriptor_fill_clock_source(void)
{
    memset(&clock_source[CLOCK_SOURCE_INDEX_STREAM], 0, sizeof(clock_source[0]));
    clock_source[CLOCK_SOURCE_INDEX_STREAM].descriptor_type = JDKSAVDECC_DESCRIPTOR_CLOCK_SOURCE;
    clock_source[CLOCK_SOURCE_INDEX_STREAM].descriptor_index = CLOCK_SOURCE_INDEX_STREAM;
    jdksavdecc_string_set_from_cstr(&clock_source[CLOCK_SOURCE_INDEX_STREAM].object_name, "AVB Stream");
    clock_source[CLOCK_SOURCE_INDEX_STREAM].localized_description = 0xffff;
    clock_source[CLOCK_SOURCE_INDEX_STREAM].clock_source_flags = 0;
    clock_source[CLOCK_SOURCE_INDEX_STREAM].clock_source_type = JDKSAVDECC_CLOCK_SOURCE_TYPE_INPUT_STREAM;
    jdksavdecc_eui64_init_from_uint64(&clock_source[CLOCK_SOURCE_INDEX_STREAM].clock_source_identifier, 0);
    clock_source[CLOCK_SOURCE_INDEX_STREAM].clock_source_location_type = JDKSAVDECC_DESCRIPTOR_STREAM_INPUT;
    clock_source[CLOCK_SOURCE_INDEX_STREAM].clock_source_location_index = 0;

    memset(&clock_source[CLOCK_SOURCE_INDEX_INTERNAL], 0, sizeof(clock_source[0]));
    clock_source[CLOCK_SOURCE_INDEX_INTERNAL].descriptor_type = JDKSAVDECC_DESCRIPTOR_CLOCK_SOURCE;
    clock_source[CLOCK_SOURCE_INDEX_INTERNAL].descriptor_index = CLOCK_SOURCE_INDEX_INTERNAL;
    jdksavdecc_string_set_from_cstr(&clock_source[CLOCK_SOURCE_INDEX_INTERNAL].object_name, "Internal (PHC/gPTP)");
    clock_source[CLOCK_SOURCE_INDEX_INTERNAL].localized_description = 0xffff;
    clock_source[CLOCK_SOURCE_INDEX_INTERNAL].clock_source_flags = 0;
    clock_source[CLOCK_SOURCE_INDEX_INTERNAL].clock_source_type = JDKSAVDECC_CLOCK_SOURCE_TYPE_INTERNAL;
    jdksavdecc_eui64_init_from_uint64(&clock_source[CLOCK_SOURCE_INDEX_INTERNAL].clock_source_identifier, 0);
    clock_source[CLOCK_SOURCE_INDEX_INTERNAL].clock_source_location_type = JDKSAVDECC_DESCRIPTOR_AUDIO_UNIT;
    clock_source[CLOCK_SOURCE_INDEX_INTERNAL].clock_source_location_index = 0;
}

void descriptor_fill_clock_domain(void)
{
    memset(&clock_domain, 0, sizeof(clock_domain));
    clock_domain.descriptor_type = JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN;
    clock_domain.descriptor_index = 0;
    jdksavdecc_string_set_from_cstr(&clock_domain.object_name, "");
    clock_domain.localized_description = 0xffff;
    clock_domain.clock_source_index = CLOCK_SOURCE_INDEX_INTERNAL;
    clock_domain.clock_sources_offset = JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN_LEN;
    clock_domain.clock_sources_count = NUM_CLOCK_SOURCES;
}

uint16_t descriptor_get_clock_source_index(void)
{
    return clock_domain.clock_source_index;
}

int descriptor_update_clock_source(void)
{
    uint16_t prev = clock_domain.clock_source_index;

    /* Always INTERNAL: the PHC paces the media clock, even when gPTP slaves to a remote GM. */
    clock_domain.clock_source_index = CLOCK_SOURCE_INDEX_INTERNAL;

    if (clock_domain.clock_source_index != prev)
        avb_log(L_AECP, LOG_DEBUG,
                "descriptor_update_clock_source: INTERNAL (PHC/gPTP media clock, FR-CLK-1)");

    return clock_domain.clock_source_index != prev;
}

void descriptor_fill_locale(void)
{
    locale.descriptor_type = JDKSAVDECC_DESCRIPTOR_LOCALE;
    locale.descriptor_index = 0;
    jdksavdecc_string_set_from_cstr(&locale.locale_identifier, "en-US");
    locale.number_of_strings = 1;
    locale.base_strings = 0;
}

void descriptor_init(uint32_t samplerate, uint64_t entity_id, uint64_t model_id, uint64_t grandmaster_id)
{
    avb_log(L_AECP, LOG_DEBUG, "descriptor_init: initializing descriptor module");

    descriptor_fill_entity();
    descriptor_fill_configuration();
    descriptor_fill_audio_unit();
    descriptor_fill_streams();
    descriptor_fill_avb_interface();
    descriptor_fill_clock_source();
    descriptor_fill_clock_domain();
    descriptor_update_clock_source();
    descriptor_fill_locale();
}

void descriptor_get_entity(void *buffer)
{
    memcpy(buffer, &entity, sizeof(entity));
}

void descriptor_get_configuration(void *buffer)
{
    memcpy(buffer, &configuration, sizeof(configuration));
    memcpy(buffer + sizeof(configuration), configuration_descriptors, sizeof(configuration_descriptors));
}

void descriptor_get_audio_unit(void *buffer)
{
    memcpy(buffer, &audio_unit, sizeof(audio_unit));
    memcpy(buffer + sizeof(audio_unit), samplerates, sizeof(samplerates));
}

void descriptor_get_stream_input(int index, void *buffer)
{
    memcpy(buffer, &stream_input[index], sizeof(stream_input[index]));
    memcpy(buffer + sizeof(stream_input[index]), &STREAM_INPUT_FORMAT_0, sizeof(STREAM_INPUT_FORMAT_0));
    memcpy(buffer + sizeof(stream_input[index]) + 8, &STREAM_INPUT_FORMAT_1, sizeof(STREAM_INPUT_FORMAT_1));
    memcpy(buffer + sizeof(stream_input[index]) + 16, &STREAM_INPUT_FORMAT_2, sizeof(STREAM_INPUT_FORMAT_2));
    memcpy(buffer + sizeof(stream_input[index]) + 24, &STREAM_INPUT_FORMAT_3, sizeof(STREAM_INPUT_FORMAT_3));
    memcpy(buffer + sizeof(stream_input[index]) + 32, &STREAM_INPUT_FORMAT_4, sizeof(STREAM_INPUT_FORMAT_4));
    memcpy(buffer + sizeof(stream_input[index]) + 40, &STREAM_INPUT_FORMAT_5, sizeof(STREAM_INPUT_FORMAT_5));
}

void descriptor_get_stream_output(int index, void *buffer)
{
    memcpy(buffer, &stream_output[index], sizeof(stream_output[index]));
    memcpy(buffer + sizeof(stream_output[index]), &STREAM_OUTPUT_FORMAT_0, sizeof(STREAM_OUTPUT_FORMAT_0));
    memcpy(buffer + sizeof(stream_output[index]) + 8, &STREAM_OUTPUT_FORMAT_1, sizeof(STREAM_OUTPUT_FORMAT_1));
    memcpy(buffer + sizeof(stream_output[index]) + 16, &STREAM_OUTPUT_FORMAT_2, sizeof(STREAM_OUTPUT_FORMAT_2));
    memcpy(buffer + sizeof(stream_output[index]) + 24, &STREAM_OUTPUT_FORMAT_3, sizeof(STREAM_OUTPUT_FORMAT_3));
    memcpy(buffer + sizeof(stream_output[index]) + 32, &STREAM_OUTPUT_FORMAT_4, sizeof(STREAM_OUTPUT_FORMAT_4));
    memcpy(buffer + sizeof(stream_output[index]) + 40, &STREAM_OUTPUT_FORMAT_5, sizeof(STREAM_OUTPUT_FORMAT_5));
}

void descriptor_get_avb_interface(void *buffer)
{
    memcpy(buffer, &avb_interface, sizeof(avb_interface));
}

void descriptor_get_clock_source(int index, void *buffer)
{
    if (index < 0 || index >= NUM_CLOCK_SOURCES)
        index = CLOCK_SOURCE_INDEX_INTERNAL;
    memcpy(buffer, &clock_source[index], sizeof(clock_source[index]));
}

void descriptor_get_clock_domain(void *buffer)
{
    memcpy(buffer, &clock_domain, sizeof(clock_domain));
}

void descriptor_get_locale(void *buffer)
{
    memcpy(buffer, &locale, sizeof(locale));
}
