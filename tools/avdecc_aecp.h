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

#ifndef AVDECC_AECP_H
#define AVDECC_AECP_H

#include <stdint.h>
#include <jdksavdecc.h>

void avdecc_aecp_init(void);

void avdecc_aecp_handle_command(int sock, char *buffer, int len);

void avdecc_aecp_set_samplerate(uint32_t rate);

void avdecc_aecp_send_set_sampling_rate(int sock, uint32_t rate);

uint32_t avdecc_aecp_get_samplerate(void);

void avdecc_aecp_init_descriptors(void);

void handle_entity_descriptor(int sock, char *buffer, int len);
void handle_configuration_descriptor(int sock, char *buffer, int len);
void handle_audio_unit_descriptor(int sock, char *buffer, int len);
void handle_stream_input_descriptor(int sock, char *buffer, int len);
void handle_stream_output_descriptor(int sock, char *buffer, int len);
void handle_avb_interface_descriptor(int sock, char *buffer, int len);
void handle_clock_source_descriptor(int sock, char *buffer, int len);
void handle_locale_descriptor(int sock, char *buffer, int len);

#endif