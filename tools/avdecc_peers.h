// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef AVDECC_PEERS_H
#define AVDECC_PEERS_H

#include <stdint.h>
#include <stddef.h>

#define AVDECC_PEERS_MAX 64

struct avdecc_peer_info {
   uint64_t eid;
   uint8_t  mac[6];
   int      departed;
   uint64_t last_adp_ms;
   uint32_t valid_ms;
   uint32_t available_index;
   uint64_t entity_model_id;
   uint32_t entity_caps;
   uint16_t talker_sources, talker_caps;
   uint16_t listener_sinks, listener_caps;
   uint32_t controller_caps;
   uint64_t gptp_gm;
   uint8_t  gptp_domain;
   int      have_aem;
   char     name[65];
   char     firmware[65];
   char     group[65];
   char     serial[65];
   uint16_t config_cur, config_count;
};

void avdecc_peers_handle_adp(const char *frame, int len);

void avdecc_peers_handle_aem_response(const char *frame, int len);

void avdecc_peers_poll(int sock, uint64_t now_ms);

int avdecc_peers_name(uint64_t entity_id, char *buf, size_t n);

int avdecc_peers_list(struct avdecc_peer_info *out, int max);

#endif
