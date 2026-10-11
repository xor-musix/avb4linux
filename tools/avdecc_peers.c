// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <jdksavdecc.h>

#include "avb_log.h"
#include "avb_status.h"
#include "utils.h"
#include "avdecc_peers.h"

extern int sendMsg(int sock, char *buffer, int buflen);

extern char MAC[6];
extern uint64_t ENTITY_ID;
extern uint64_t CONTROLLER_ID;

#define HEADER_OFFSET 14
#define MACLEN 6

#define MAX_PEERS        AVDECC_PEERS_MAX
#define NAME_RETRY_MS    1000
#define NAME_MAX_TRIES   5
#define NAME_REFRESH_MS  60000

struct peer {
   struct avdecc_peer_info info;
   int      mac_valid;
   int      tries;
   uint16_t seq;
   uint64_t next_ms;
};

static struct peer g_peers[MAX_PEERS];
static int g_n_peers;
static uint16_t g_seq;
static pthread_mutex_t g_peers_lock = PTHREAD_MUTEX_INITIALIZER;

static struct peer *peer_find(uint64_t eid)
{
   for (int i = 0; i < g_n_peers; i++)
      if (g_peers[i].info.eid == eid)
         return &g_peers[i];
   return NULL;
}

void avdecc_peers_handle_adp(const char *frame, int len)
{
   struct jdksavdecc_adpdu adp;

   if (jdksavdecc_adpdu_read(&adp, frame, HEADER_OFFSET, len) < 0)
      return;
   uint64_t eid = jdksavdecc_eui64_convert_to_uint64(&adp.header.entity_id);
   if (eid == ENTITY_ID)
      return;

   pthread_mutex_lock(&g_peers_lock);
   struct peer *p = peer_find(eid);
   switch (adp.header.message_type) {
   case JDKSAVDECC_ADP_MESSAGE_TYPE_ENTITY_AVAILABLE:
      if (!p && g_n_peers < MAX_PEERS) {
         p = &g_peers[g_n_peers++];
         memset(p, 0, sizeof *p);
         p->info.eid = eid;
      }
      if (!p)
         break;
      memcpy(p->info.mac, frame + MACLEN, MACLEN);
      if (!p->mac_valid || adp.available_index < p->info.available_index) {
         p->tries = 0;
         p->next_ms = 0;
      }
      p->mac_valid = 1;
      p->info.available_index = adp.available_index;
      p->info.last_adp_ms     = get_time_in_ms();
      p->info.valid_ms        = (adp.header.valid_time ? adp.header.valid_time : 31) * 2000u;
      p->info.entity_model_id = jdksavdecc_eui64_convert_to_uint64(&adp.entity_model_id);
      p->info.entity_caps     = adp.entity_capabilities;
      p->info.talker_sources  = adp.talker_stream_sources;
      p->info.talker_caps     = adp.talker_capabilities;
      p->info.listener_sinks  = adp.listener_stream_sinks;
      p->info.listener_caps   = adp.listener_capabilities;
      p->info.controller_caps = adp.controller_capabilities;
      p->info.gptp_gm         = jdksavdecc_eui64_convert_to_uint64(&adp.gptp_grandmaster_id);
      p->info.gptp_domain     = adp.gptp_domain_number;
      break;
   case JDKSAVDECC_ADP_MESSAGE_TYPE_ENTITY_DEPARTING:
      if (p)
         p->mac_valid = 0;
      break;
   default:
      break;
   }
   pthread_mutex_unlock(&g_peers_lock);
}

static void copy_name(char *dst, const uint8_t *src)
{
   int i;
   for (i = 0; i < 64 && src[i]; i++)
      dst[i] = src[i] < 0x20 || src[i] == 0x7f ? '?' : (char)src[i];
   dst[i] = '\0';
}

void avdecc_peers_handle_aem_response(const char *frame, int len)
{
   const int d = HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR_RESPONSE_OFFSET_DESCRIPTOR;
   struct jdksavdecc_eui64 v;

   if (len < HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR_RESPONSE_LEN)
      return;
   v = jdksavdecc_aecpdu_aem_get_controller_entity_id(frame, HEADER_OFFSET);
   if (jdksavdecc_eui64_convert_to_uint64(&v) != CONTROLLER_ID)
      return;
   /* top bit of command_type is the unsolicited flag */
   if ((jdksavdecc_aecpdu_aem_get_command_type(frame, HEADER_OFFSET) & 0x7fff)
       != JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR)
      return;
   if (jdksavdecc_common_control_header_get_status(frame, HEADER_OFFSET) == JDKSAVDECC_AEM_STATUS_SUCCESS &&
       len >= d + 2 && jdksavdecc_descriptor_entity_get_descriptor_type(frame, d) != JDKSAVDECC_DESCRIPTOR_ENTITY)
      return;

   v = jdksavdecc_common_control_header_get_stream_id(frame, HEADER_OFFSET);
   uint64_t eid = jdksavdecc_eui64_convert_to_uint64(&v);
   uint16_t seq = jdksavdecc_aecpdu_aem_get_sequence_id(frame, HEADER_OFFSET);
   uint8_t status = jdksavdecc_common_control_header_get_status(frame, HEADER_OFFSET);
   int complete = len >= d + JDKSAVDECC_DESCRIPTOR_ENTITY_OFFSET_ENTITY_NAME + 64 &&
                  jdksavdecc_descriptor_entity_get_descriptor_type(frame, d) == JDKSAVDECC_DESCRIPTOR_ENTITY;
   int full = len >= d + JDKSAVDECC_DESCRIPTOR_ENTITY_LEN;
   char name[65], old[65] = "";
   int changed = 0;

   pthread_mutex_lock(&g_peers_lock);
   struct peer *p = peer_find(eid);
   if (!p || seq != p->seq) {
      pthread_mutex_unlock(&g_peers_lock);
      return;
   }
   if (status == JDKSAVDECC_AEM_STATUS_SUCCESS && complete) {
      struct jdksavdecc_string s = jdksavdecc_descriptor_entity_get_entity_name(frame, d);
      copy_name(name, s.value);
      changed = !p->info.have_aem || strcmp(p->info.name, name);
      if (p->info.have_aem)
         strcpy(old, p->info.name);
      strcpy(p->info.name, name);
      if (full) {
         s = jdksavdecc_descriptor_entity_get_firmware_version(frame, d);
         copy_name(p->info.firmware, s.value);
         s = jdksavdecc_descriptor_entity_get_group_name(frame, d);
         copy_name(p->info.group, s.value);
         s = jdksavdecc_descriptor_entity_get_serial_number(frame, d);
         copy_name(p->info.serial, s.value);
         p->info.config_cur   = jdksavdecc_descriptor_entity_get_current_configuration(frame, d);
         p->info.config_count = jdksavdecc_descriptor_entity_get_configurations_count(frame, d);
      }
      p->info.have_aem = 1;
      p->tries = 0;
      p->next_ms = get_time_in_ms() + NAME_REFRESH_MS;
   }
   pthread_mutex_unlock(&g_peers_lock);

   if (status != JDKSAVDECC_AEM_STATUS_SUCCESS || !complete)
      avb_log(L_AVDECC, LOG_DEBUG, "entity %016llx: READ_DESCRIPTOR(ENTITY) status %u, %d bytes",
              (unsigned long long)eid, status, len);
   else if (changed && old[0])
      avb_log(L_AVDECC, LOG_INFO, "entity %016llx renamed \"%s\" -> \"%s\"",
              (unsigned long long)eid, old, name);
   else if (changed)
      avb_log(L_AVDECC, LOG_INFO, "entity %016llx is \"%s\"", (unsigned long long)eid, name);
}

static void send_read_entity(int sock, const uint8_t *mac, uint64_t eid, uint16_t seq)
{
   char buffer[HEADER_OFFSET + JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR_COMMAND_LEN];
   struct jdksavdecc_aem_command_read_descriptor cmd;

   memset(&cmd, 0, sizeof cmd);
   cmd.aem_header.aecpdu_header.header.cd = 1;
   cmd.aem_header.aecpdu_header.header.subtype = JDKSAVDECC_SUBTYPE_AECP;
   cmd.aem_header.aecpdu_header.header.message_type = JDKSAVDECC_AECP_MESSAGE_TYPE_AEM_COMMAND;
   cmd.aem_header.aecpdu_header.header.status = JDKSAVDECC_AEM_STATUS_SUCCESS;
   cmd.aem_header.aecpdu_header.header.control_data_length =
      JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR_COMMAND_LEN - JDKSAVDECC_COMMON_CONTROL_HEADER_LEN;
   jdksavdecc_eui64_init_from_uint64(&cmd.aem_header.aecpdu_header.header.target_entity_id, eid);
   jdksavdecc_eui64_init_from_uint64(&cmd.aem_header.aecpdu_header.controller_entity_id, CONTROLLER_ID);
   cmd.aem_header.aecpdu_header.sequence_id = seq;
   cmd.aem_header.command_type = JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR;
   cmd.configuration_index = 0;
   cmd.descriptor_type = JDKSAVDECC_DESCRIPTOR_ENTITY;
   cmd.descriptor_index = 0;

   memcpy(buffer, mac, MACLEN);
   memcpy(&buffer[MACLEN], MAC, MACLEN);
   buffer[12] = 0x22; buffer[13] = 0xf0;
   jdksavdecc_aem_command_read_descriptor_write(&cmd, buffer, HEADER_OFFSET, sizeof buffer);

   sendMsg(sock, buffer, sizeof buffer);
}

void avdecc_peers_poll(int sock, uint64_t now_ms)
{
   uint64_t want[2 * MAX_STREAMS];
   int n_want = 0;

   pthread_mutex_lock(&g_status_lock);
   for (int i = 0; i < g_status.n_streams; i++) {
      for (int dir = 0; dir < 2; dir++) {
         int bound   = dir ? g_status.conn[i].rx_bound : g_status.conn[i].tx_bound;
         uint64_t id = dir ? g_status.conn[i].rx_peer  : g_status.conn[i].tx_peer;
         int j;
         if (!bound)
            continue;
         for (j = 0; j < n_want && want[j] != id; j++)
            ;
         if (j == n_want)
            want[n_want++] = id;
      }
   }
   pthread_mutex_unlock(&g_status_lock);

   struct { uint8_t mac[MACLEN]; uint64_t eid; uint16_t seq; } req[MAX_PEERS];
   int n_req = 0;

   pthread_mutex_lock(&g_peers_lock);
   for (int i = 0; i < g_n_peers; i++) {
      struct peer *p = &g_peers[i];
      int j;
      for (j = 0; j < n_want && want[j] != p->info.eid; j++)
         ;
      if (j == n_want && !(p->info.entity_caps & JDKSAVDECC_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED))
         continue;
      if (!p->mac_valid || now_ms < p->next_ms)
         continue;
      if (p->tries >= NAME_MAX_TRIES) {
         if (!p->info.have_aem)
            avb_log(L_AVDECC, LOG_INFO, "entity %016llx: no answer to READ_DESCRIPTOR(ENTITY), retrying in %ds",
                    (unsigned long long)p->info.eid, NAME_REFRESH_MS / 1000);
         p->tries = 0;
         p->next_ms = now_ms + NAME_REFRESH_MS;
         continue;
      }
      p->seq = ++g_seq;
      p->tries++;
      p->next_ms = now_ms + NAME_RETRY_MS;
      memcpy(req[n_req].mac, p->info.mac, MACLEN);
      req[n_req].eid = p->info.eid;
      req[n_req].seq = p->seq;
      n_req++;
   }
   pthread_mutex_unlock(&g_peers_lock);

   for (int i = 0; i < n_req; i++)
      send_read_entity(sock, req[i].mac, req[i].eid, req[i].seq);
}

int avdecc_peers_name(uint64_t entity_id, char *buf, size_t n)
{
   int ok;

   pthread_mutex_lock(&g_peers_lock);
   struct peer *p = peer_find(entity_id);
   ok = p && p->info.have_aem && p->info.name[0];
   if (ok)
      snprintf(buf, n, "%s", p->info.name);
   pthread_mutex_unlock(&g_peers_lock);
   return ok;
}

int avdecc_peers_list(struct avdecc_peer_info *out, int max)
{
   int n;

   pthread_mutex_lock(&g_peers_lock);
   n = g_n_peers < max ? g_n_peers : max;
   for (int i = 0; i < n; i++) {
      out[i] = g_peers[i].info;
      out[i].departed = !g_peers[i].mac_valid;
   }
   pthread_mutex_unlock(&g_peers_lock);
   return n;
}
