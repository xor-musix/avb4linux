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

#ifndef DESCRIPTOR_H
#define DESCRIPTOR_H

#include <stdint.h>
#include <jdksavdecc.h>

#define CLOCK_SOURCE_INDEX_STREAM   0
#define CLOCK_SOURCE_INDEX_INTERNAL 1
#define NUM_CLOCK_SOURCES           2

extern int g_num_streams;
#define STREAM_PORT_INPUT_BASE_CLUSTER 0
#define STREAM_PORT_INPUT_BASE_MAP 0
#define STREAM_PORT_OUTPUT_BASE_CLUSTER (STREAM_PORT_INPUT_BASE_CLUSTER+(g_num_streams*8))
#define STREAM_PORT_OUTPUT_BASE_MAP (STREAM_PORT_INPUT_BASE_MAP+g_num_streams)

void descriptor_set_entity_name(const char *hostname);

const char *descriptor_get_entity_name(void);

void descriptor_init(uint32_t samplerate, uint64_t entity_id, uint64_t model_id, uint64_t grandmaster_id);

void descriptor_get_entity(void *buffer);
void descriptor_get_configuration(void *buffer);
void descriptor_get_audio_unit(void *buffer);
void descriptor_get_stream_input(int index, void *buffer);
void descriptor_get_stream_output(int index, void *buffer);
void descriptor_get_avb_interface(void *buffer);
void descriptor_get_clock_source(int index, void *buffer);
void descriptor_get_clock_domain(void *buffer);
void descriptor_get_locale(void *buffer);

uint16_t descriptor_get_clock_source_index(void);

int descriptor_update_clock_source(void);

#endif