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

#ifndef AVDECC_ACMP_H
#define AVDECC_ACMP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <jdksavdecc.h>

struct acmp_conn {
    int      bound;
    int      source;
    uint64_t talker_entity_id, listener_entity_id;
    uint16_t talker_unique_id, listener_unique_id;
    uint64_t stream_id;
    uint64_t dest_mac;
    uint16_t flags;
    uint16_t connection_count;
};

void avdecc_acmp_init(uint64_t entity_id, uint64_t controller_id, uint64_t grandmaster_id);
void avdecc_acmp_handle_command(int sock, char *buffer, int len);

void acmp_conns_init(void);
void acmp_render_summary(char *buf, size_t n);

struct acmp_conn acmp_tx_get(int idx);
struct acmp_conn acmp_rx_get(int idx);

void avdecc_acmp_connect(int sock);

void acmp_tick(int sock, uint64_t now_ms);

void handle_acmp_connect_tx_command(int sock, char *buffer, int len);
void handle_acmp_connect_tx_response(int sock, char *buffer, int len);
void handle_acmp_get_rx_state_command(int sock, char *buffer, int len);
void handle_acmp_get_tx_state_command(int sock, char *buffer, int len);
void handle_acmp_get_tx_connection_command(int sock, char *buffer, int len);
void handle_acmp_connect_rx_command(int sock, char *buffer, int len);
void handle_acmp_connect_rx_response(int sock, char *buffer, int len);
void handle_acmp_get_rx_state_response(int sock, char *buffer, int len);
void handle_acmp_get_tx_connection_response(int sock, char *buffer, int len);

void handle_acmp_disconnect_tx_command(int sock, char *buffer, int len);
void handle_acmp_disconnect_rx_command(int sock, char *buffer, int len);

#endif
