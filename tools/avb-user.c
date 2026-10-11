/******************************************************************************

  Copyright (c) 2019, Ralf Beck
  Copyright (c) 2026, mnemonic powerplay <mnemonic@xor-musix.de>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

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
#include "avb_status.h"
#include "gptp_client.h"
#include "gptp_link.h"
#include "utils.h"
#include "../include/stream_id.h"
#include "stream_identity.h"
#include "avtp.h"
#include "msrp_talker.h"
#include "msrp_listener.h"
#include "msrp_domain.h"
#include "avdecc_adp.h"
#include "avdecc_aecp.h"
#include "avdecc_acmp.h"
#include "avdecc_peers.h"
#include "avdecc_probe.h"
#include "descriptor.h"

#include <jdksavdecc.h>
#include <../kernel-module/snd-igb-avb/src/avb-config.h>

char DMAC[] = { 0x91, 0xe0, 0xf0, 0x01, 0x00, 0x00 };

#define MCAST_BASE MAAP_LOCALLY_ADMINISTRATORED_BASE

uint64_t g_device_source_mac = 0;
uint64_t g_device_talker_mac = 0;

static struct maap_record g_maap_ctx = { .base = 0, .count = 0 };

int g_num_streams = 1;
struct avb_stream_id_cfg g_streams[MAX_STREAMS];

uint32_t samplerate = 48000;

uint64_t ENTITY_ID;
uint64_t CONTROLLER_ID;
uint64_t MODEL_ID;
uint64_t GRANDMASTER_ID;

uint64_t own_mac_address;
char MAC[6];

struct jdksavdecc_eui64 entity_guid;

int ifindex = 0;

volatile sig_atomic_t running = 1;
unsigned global_available_index = 0;

static void on_signal(int sig)
{
   (void)sig;
   running = 0;
}

static void publish_talker_dest_macs_to_kernel(void)
{
   static const char path[] =
      "/sys/module/snd_igb_avb/parameters/avb_talker_dest_macs";
   char buf[640];  /* 32 x 17 + 31 commas = 575 */
   int pos = 0;
   FILE *f;
   int i;

   for (i = 0; i < g_num_streams; i++) {
       uint8_t m[6];
       if (i > 0) {
           buf[pos++] = ',';
       }
       uint64_to_array6(g_streams[i].dest_mac, m);
       pos += snprintf(buf + pos, sizeof(buf) - pos,
                       "%02x:%02x:%02x:%02x:%02x:%02x",
                       m[0], m[1], m[2], m[3], m[4], m[5]);
       if (pos < 0 || pos >= sizeof(buf)) {
           avb_log(L_AVTP, LOG_WARNING, "buffer overflow building dest_macs");
           return;
       }
   }

   f = fopen(path, "w");
   if (!f) {
       avb_log(L_AVTP, LOG_WARNING, "cannot open %s (%s); kernel keeps baked dest",
               path, strerror(errno));
       return;
   }
   if (fputs(buf, f) < 0)
       avb_log(L_AVTP, LOG_WARNING, "failed writing talker dest_macs to kernel");
   else
       avb_log(L_AVTP, LOG_DEBUG, "published %d talker dest_macs to kernel", g_num_streams);
   fclose(f);
}

static int kernel_param_exists(const char *name)
{
   char path[128];
   struct stat st;

   snprintf(path, sizeof(path), "/sys/module/snd_igb_avb/parameters/%s", name);
   return stat(path, &st) == 0 || errno != ENOENT;
}

static int write_kernel_param(const char *name, const char *val)
{
   char path[128];
   FILE *f;
   int err;

   snprintf(path, sizeof(path), "/sys/module/snd_igb_avb/parameters/%s", name);
   f = fopen(path, "w");
   if (!f)
      return -1;
   err = fputs(val, f) < 0;
   if (fclose(f))
      err = 1;
   return err ? -1 : 0;
}

static int listener_valid(const struct acmp_conn *rx)
{
   return rx->bound && rx->dest_mac && rx->stream_id && rx->stream_id != ~0ULL;
}

static void publish_listener_identities_to_kernel(void)
{
   static char last_dst[640], last_src[640], last_sid[640];  /* 32 x 17 + 31 commas */
   static uint64_t logged_sid[MAX_STREAMS];
   static int published, warned, no_sid_param;
   char dst[640], src[640], sid[640];
   int dp = 0, sp = 0, ip = 0;
   uint32_t uid_mismatch = 0;
   int bound = 0;

   dst[0] = src[0] = sid[0] = '\0';
   for (int k = 0; k < g_num_streams; k++) {
      struct acmp_conn rx = acmp_rx_get(k);

      if (k) {
         dp += snprintf(dst + dp, sizeof(dst) - dp, ",");
         sp += snprintf(src + sp, sizeof(src) - sp, ",");
         ip += snprintf(sid + ip, sizeof(sid) - ip, ",");
      }
      if (listener_valid(&rx)) {
         uint8_t d[6], s[6];

         uint64_to_array6(rx.dest_mac, d);
         uint64_to_array6(rx.stream_id >> 16, s);
         dp += snprintf(dst + dp, sizeof(dst) - dp,
                        "%02x:%02x:%02x:%02x:%02x:%02x",
                        d[0], d[1], d[2], d[3], d[4], d[5]);
         sp += snprintf(src + sp, sizeof(src) - sp,
                        "%02x:%02x:%02x:%02x:%02x:%02x",
                        s[0], s[1], s[2], s[3], s[4], s[5]);
         ip += snprintf(sid + ip, sizeof(sid) - ip, "%016llx",
                        (unsigned long long)rx.stream_id);
         if ((rx.stream_id & 0xffff) != (uint64_t)k)
            uid_mismatch |= 1u << k;
         bound++;
      }
      if (dp >= (int)sizeof(dst) || sp >= (int)sizeof(src) || ip >= (int)sizeof(sid)) {
         avb_log(L_AVDECC, LOG_WARNING, "buffer overflow building listener identities");
         return;
      }
   }

   if (published && !strcmp(dst, last_dst) && !strcmp(src, last_src) &&
       !strcmp(sid, last_sid))
      return;

   /* dest first, src last: emptying a dest unbinds the stream on the first write */
   if (write_kernel_param("avb_listener_dest_macs", dst))
      goto fail;
   if (!no_sid_param && !kernel_param_exists("avb_listener_stream_ids")) {
      no_sid_param = 1;
      avb_log(L_AVDECC, LOG_WARNING,
              "kernel module has no avb_listener_stream_ids (older module?): "
              "publishing MACs only, the kernel derives stream id src<<16|k");
   }
   if (!no_sid_param && write_kernel_param("avb_listener_stream_ids", sid))
      goto fail;
   if (write_kernel_param("avb_listener_src_macs", src))
      goto fail;
   warned = 0;
   memcpy(last_dst, dst, sizeof(dst));
   memcpy(last_src, src, sizeof(src));
   memcpy(last_sid, sid, sizeof(sid));
   published = 1;
   avb_log(L_AVDECC, LOG_INFO, "published listener MACs (%d/%d bound): dest=%s src=%s",
           bound, g_num_streams, dst, src);
   for (int k = 0; k < g_num_streams; k++) {
      struct acmp_conn rx = acmp_rx_get(k);
      uint64_t id = listener_valid(&rx) ? rx.stream_id : 0;

      if (id == logged_sid[k])
         continue;
      logged_sid[k] = id;
      if (!id)
         continue;
      if (!no_sid_param)
         avb_log(L_AVDECC, LOG_INFO, "listener %d <- talker %016llx", k,
                 (unsigned long long)id);
      else if (uid_mismatch & (1u << k))
         avb_log(L_AVDECC, LOG_WARNING,
                 "listener[%d] is connected to talker source %llu, not %d: the kernel expects stream id src<<16|%d and will drop it",
                 k, (unsigned long long)(id & 0xffff), k, k);
   }
   return;

fail:
   if (!warned)
      avb_log(L_AVDECC, LOG_WARNING,
              "cannot publish listener identities to kernel (%s); streams 1..N-1 stay unbound",
              strerror(errno));
   warned = 1;
}

static void publish_sync_state_to_kernel(int synced, int64_t offset_ns)
{
   FILE *f = fopen("/proc/snd_igb_avb/tx_engine", "w");

   if (!f) {
      static int warned;
      if (!warned) {
         avb_log(L_AVTP, LOG_WARNING, "cannot publish gPTP sync state to kernel (%s)",
                 strerror(errno));
         warned = 1;
      }
      return;
   }
   fprintf(f, "sync_state=%u offset_ns=%lld\n", synced ? 1 : 0,
           (long long)offset_ns);
   fclose(f);
}


/* must equal the per-frame overhead the kernel's avb_stream_idleslope_bps() adds */
#define MSRP_PER_FRAME_OVERHEAD_BYTES 42

static uint64_t stream_reserved_bps(int pktsz, int interval)
{
    return (uint64_t)(pktsz + MSRP_PER_FRAME_OVERHEAD_BYTES) * 8 * 8000 * (uint64_t)interval;
}

static int clamp_stream_count(long n)
{
   if (n < 1)
      return 1;
   if (n > MAX_STREAMS)
      return MAX_STREAMS;
   return (int)n;
}

int main(int argc, char **argv)
{
   struct sockaddr mac;

   uint32_t channels_per_stream = CHANNELS_PER_STREAM;
   uint32_t bytes_per_sample = 4;
   uint32_t samples_per_interval = 6;

   if (argc < 3) {
      fprintf(stderr, "usage: avb-user <name of ethernet interface> <samplerate> [num_streams]\n");
      exit(1);
   }

   avb_log_init();
   signal(SIGINT,  on_signal);
   signal(SIGTERM, on_signal);
   avb_log(L_SYS, LOG_INFO, "avb-user starting on %s", argv[1]);

   g_device_source_mac = parse_mac_u64(getenv("AVB_DEVICE_SOURCE_MAC"), g_device_source_mac);
   g_device_talker_mac = parse_mac_u64(getenv("AVB_DEVICE_TALKER_MAC_BASE"), g_device_talker_mac);

   if (!g_device_source_mac || !g_device_talker_mac) {
      avb_log(L_SYS, LOG_ERR, "AVB_DEVICE_SOURCE_MAC and AVB_DEVICE_TALKER_MAC_BASE "
                              "must be set (no default -- they are individual per device)");
      fprintf(stderr, "error: AVB_DEVICE_SOURCE_MAC and AVB_DEVICE_TALKER_MAC_BASE "
                      "must be set (no default -- they are individual per device)\n");
      return -1;
   }

   samplerate = atoi(argv[2]);
   switch (samplerate) {
      case 44100:
      case 48000:  samples_per_interval = 6;  break;
      case 88200:
      case 96000:  samples_per_interval = 12; break;
      case 176400:
      case 192000: samples_per_interval = 24; break;
      default:
         avb_log(L_SYS, LOG_ERR, "unsupported samplerate %d", samplerate);
         fprintf(stderr, "error: unsupported samplerate %d\n", samplerate);
         exit(1);
   }

   {
      const char *env_streams = getenv("AVB_NUM_STREAMS");
      long n = 1;

      if (env_streams && env_streams[0]) {
         char *end = NULL;
         n = strtol(env_streams, &end, 10);
         if (end == env_streams || *end != '\0') {
            avb_log(L_SYS, LOG_WARNING, "invalid AVB_NUM_STREAMS=%s, using 1", env_streams);
            n = 1;
         }
      }
      if (argc >= 4) {
         char *end = NULL;
         n = strtol(argv[3], &end, 10);
         if (end == argv[3] || *end != '\0')
            avb_log(L_SYS, LOG_WARNING, "invalid stream count %s, using %d", argv[3], g_num_streams);
      }
      g_num_streams = clamp_stream_count(n);
      if (g_num_streams != n) {
         avb_log(L_SYS, LOG_WARNING, "stream count %ld clamped to %d", n, g_num_streams);
      }
   }

   int sock = avtp_socket_create(argv[1], &mac);
   if (sock < 0) {
      avb_log(L_SYS, LOG_ERR, "failed to create AVTP socket");
      return -1;
   }
   ifindex = avtp_ifindex;

    memcpy(MAC, &mac.sa_data, MACLEN);
    own_mac_address = array6_to_uint64(MAC);

    {
        char hostname[64];
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            descriptor_set_entity_name(hostname);
            avb_log(L_SYS, LOG_INFO, "entity name: %s AVB", hostname);
        } else {
            avb_log(L_SYS, LOG_WARNING, "gethostname failed, using default entity name");
        }
    }

   for (int i = 0; i < g_num_streams; i++) {
      g_streams[i].local_mac = own_mac_address;
      g_streams[i].peer_mac = g_device_source_mac;
      g_streams[i].dest_mac = MCAST_BASE;
      g_streams[i].stream_id = AVB_STREAM_ID(own_mac_address, i);
      g_streams[i].peer_entity_id = mac_to_entity_id(g_device_source_mac);
   }

   ENTITY_ID      = mac_to_entity_id(own_mac_address);
   CONTROLLER_ID  = mac_to_controller_id(own_mac_address);
   /* controllers cache the AEM by entity_model_id, so N must be folded into it */
   MODEL_ID       = 0xa08cfd0000000005ULL + ((uint64_t)(g_num_streams - 1) << 16);
   GRANDMASTER_ID = mac_to_entity_id(own_mac_address);
   jdksavdecc_eui64_init_from_uint64(&entity_guid, ENTITY_ID);

    gptp_client_init();

    int talker_dest_lvl = MAAP_LVL_DYNAMIC;
    uint64_t dest_base = MCAST_BASE;
    unsigned dest_count = g_num_streams;
    if (getenv("AVB_MAAP") && strcmp(getenv("AVB_MAAP"), "0") == 0) {
        status_update_maap(MCAST_BASE, MAAP_LVL_FIXED, g_num_streams);
        talker_dest_lvl = MAAP_LVL_FIXED;
    } else {
        g_maap_ctx.base = MCAST_BASE;
        g_maap_ctx.count = g_num_streams;
        int maap_rc = maap_client_init(&g_maap_ctx);
        if (maap_rc == 0) {
            maap_rc = maap_acquire_adresses(&g_maap_ctx);
        }
        if (maap_rc == 0) {
            talker_dest_lvl = MAAP_LVL_DYNAMIC;
            dest_base = g_maap_ctx.base;
            dest_count = g_maap_ctx.count;
            status_update_maap(g_maap_ctx.base, MAAP_LVL_DYNAMIC, g_maap_ctx.count);
            avb_log(L_AVDECC, LOG_INFO, "MAAP allocated block base=0x%012llx count=%u (requested %d)",
                    (unsigned long long)g_maap_ctx.base, g_maap_ctx.count, g_num_streams);
        } else {
            g_maap_ctx.count = 0;
            talker_dest_lvl = MAAP_LVL_FALLBACK;
            status_update_maap(MCAST_BASE, MAAP_LVL_FALLBACK, g_num_streams);
            avb_log(L_AVDECC, LOG_WARNING, "MAAP unavailable, using fixed base 0x%012llx (fallback)",
                    (unsigned long long)MCAST_BASE);
        }
    }

    if (dest_count < (unsigned)g_num_streams)
        avb_log(L_AVDECC, LOG_ERR, "MAAP granted only %u of %d addresses (base=0x%012llx): streams %u..%d use the UNRESERVED fixed base 0x%012llx + index",
                dest_count, g_num_streams, (unsigned long long)dest_base,
                dest_count, g_num_streams - 1, (unsigned long long)MCAST_BASE);
    for (int i = 0; i < g_num_streams; i++) {
        uint64_t stream_dest = ((unsigned)i < dest_count ? dest_base : MCAST_BASE) + i;
        stream_dest &= 0xffffffffffffULL;
        g_streams[i].dest_mac = stream_dest;
        avb_log(L_AVDECC, LOG_DEBUG, "stream[%d] dest_mac=0x%012llx (from block base=0x%012llx)",
                i, (unsigned long long)stream_dest, (unsigned long long)dest_base);
    }

    publish_talker_dest_macs_to_kernel();
    avb_log(L_AVTP, LOG_INFO, "talker dest block base=0x%012llx count=%u (%s)",
            (unsigned long long)dest_base, dest_count,
            talker_dest_lvl == MAAP_LVL_DYNAMIC ? "MAAP" : "fixed");

    msrp_domain_init();
   msrp_talker_init();
   msrp_listener_init();
   avdecc_adp_init(ENTITY_ID, MODEL_ID, GRANDMASTER_ID);
   avdecc_aecp_init();
   avdecc_aecp_init_descriptors();
   avdecc_acmp_init(ENTITY_ID, CONTROLLER_ID, GRANDMASTER_ID);
   acmp_conns_init();
   descriptor_init(samplerate, ENTITY_ID, MODEL_ID, GRANDMASTER_ID);
   avdecc_aecp_set_samplerate(samplerate);

   avb_log(L_AVDECC, LOG_INFO, "advertising %d streams (model_id=%016llx)",
           g_num_streams, (unsigned long long)MODEL_ID);
    for (int i = 0; i < g_num_streams; i++) {
         avb_log(L_AVDECC, LOG_INFO, "stream[%d] local=%012llx peer=%012llx dest=%012llx sid=%016llx",
                 i,
                 (unsigned long long)g_streams[i].local_mac,
                 (unsigned long long)g_streams[i].peer_mac,
                 (unsigned long long)g_streams[i].dest_mac,
                 (unsigned long long)g_streams[i].stream_id);
    }

     {
         struct avb_stream s[MAX_STREAMS];
         memset(s, 0, sizeof(s));
         for (int i = 0; i < g_num_streams; i++) {
             s[i].stream_id = g_streams[i].stream_id;
             s[i].dest_mac[0] = (g_streams[i].dest_mac >> 40) & 0xff;
             s[i].dest_mac[1] = (g_streams[i].dest_mac >> 32) & 0xff;
             s[i].dest_mac[2] = (g_streams[i].dest_mac >> 24) & 0xff;
             s[i].dest_mac[3] = (g_streams[i].dest_mac >> 16) & 0xff;
             s[i].dest_mac[4] = (g_streams[i].dest_mac >> 8) & 0xff;
             s[i].dest_mac[5] = g_streams[i].dest_mac & 0xff;
             s[i].vlan = 0x6002;
             s[i].sequence = 0;
             s[i].dbc = 0;
             s[i].format = 0;
             s[i].channels = CHANNELS_PER_STREAM;
             s[i].samplerate = samplerate;
         }
         status_update_streams(g_num_streams, s, NULL);
     }

   avb_log(L_AVTP, LOG_INFO, "talker stream-id=%016llx listener stream-id=%016llx",
           (unsigned long long)AVB_STREAM_ID(own_mac_address, 0),
           (unsigned long long)AVB_STREAM_ID(g_device_source_mac, 0));

   int halt_mrp = 0;
   int rc;

   mrp_client_init(msrp_listener_on_talker_declaration,
                   msrp_talker_on_listener_declaration,
                   my_domain_cb);
   mrp_connect(&halt_mrp);

   rc = msrp_domain_register(MSRP_CLASS_A_ID, MSRP_CLASS_A_PRIORITY, MSRP_CLASS_A_VLAN);
   avb_log(L_MSRP, LOG_DEBUG, "msrp_domain_register rc=%d", rc);

   rc = msrp_domain_join_vlan(MSRP_CLASS_A_ID, MSRP_CLASS_A_PRIORITY, MSRP_CLASS_A_VLAN);
   avb_log(L_MSRP, LOG_DEBUG, "msrp_domain_join_vlan (MVRP) rc=%d", rc);

   avdecc_aecp_send_set_sampling_rate(sock, samplerate);
   sleep(5);
   avb_log(L_AVDECC, LOG_DEBUG, "done setting remote samplerate");

   pthread_t control_thread_id;
   pthread_create(&control_thread_id, NULL, avtp_message_handler, &sock);

   avb_log(L_SYS, LOG_INFO, "own MAC %02x:%02x:%02x:%02x:%02x:%02x",
           (uint8_t)mac.sa_data[0], (uint8_t)mac.sa_data[1], (uint8_t)mac.sa_data[2],
           (uint8_t)mac.sa_data[3], (uint8_t)mac.sa_data[4], (uint8_t)mac.sa_data[5]);

   avdecc_acmp_connect(sock);

   static struct msrp_talker_ctx talker_ctx[MAX_STREAMS];
   static struct msrp_listener_ctx listener_ctx[MAX_STREAMS];

   for (int k = 0; k < g_num_streams; k++) {
       uint8_t ox_stream[8], ox_mac[6];
       uint64_to_array6(g_streams[k].dest_mac, ox_mac);
       uint64_to_array8(g_streams[k].stream_id, ox_stream);

       talker_ctx[k].idx = k;
       talker_ctx[k].pktsz = 32 + channels_per_stream * bytes_per_sample * samples_per_interval;
       talker_ctx[k].interval = 1;
       talker_ctx[k].latency = 95;
       talker_ctx[k].vid = MSRP_CLASS_A_VLAN;
       talker_ctx[k].priority = MSRP_CLASS_A_PRIORITY;
       talker_ctx[k].last_ms = 0;
       talker_ctx[k].attempts = 0;
       memcpy(talker_ctx[k].ox_stream, ox_stream, 8);
       memcpy(talker_ctx[k].ox_mac, ox_mac, 6);
       msrp_talker_start(&talker_ctx[k]);

       uint8_t ix_stream[8], ix_mac[6];
       uint64_t ix = msrp_listener_expected_stream_id(k);
       uint64_to_array6(g_streams[k].peer_mac, ix_mac);
       uint64_to_array8(ix, ix_stream);

       listener_ctx[k].idx = k;
       listener_ctx[k].last_ms = 0;
       listener_ctx[k].attempts = 0;
       memcpy(listener_ctx[k].ix_stream, ix_stream, 8);
       memcpy(listener_ctx[k].ix_mac, ix_mac, 6);
       if (ix)
           msrp_listener_start(&listener_ctx[k]);
   }

   avb_log(L_AVTP, LOG_INFO, "kernel relay is the audio data plane");

   status_init();
   gptp_link_init(ifindex);

   uint64_t last_adp = 0, last_render = 0, start_ms = get_time_in_ms();
   uint64_t last_tx_ticks = 0, last_tx_ms = 0;
   int was_locked = 0, was_synced = 0;
   int last_kernel_synced = -1;
   int last_bw_match = -2;
   uint64_t was_gm = 0;

   uint64_t msrp_reserved_bps = 0;
   for (int k = 0; k < g_num_streams; k++)
       msrp_reserved_bps += stream_reserved_bps(talker_ctx[k].pktsz, talker_ctx[k].interval);

   while (running) {
      uint64_t now = get_time_in_ms();

      if (now - last_adp > 2000) {
         avdecc_adp_send_announce(sock);
         avdecc_adp_update_last_adp(now);
         last_adp = now;
      }

        gptp_poll(&g_status, &g_status_lock, &was_locked, &was_synced, &was_gm);
        gptp_link_poll(now);
        {
            int synced;
            int64_t offset;
            pthread_mutex_lock(&g_status_lock);
            synced = g_status.gptp_synced;
            offset = g_status.gptp_offset_ns;
            pthread_mutex_unlock(&g_status_lock);
            if (synced != last_kernel_synced) {
                publish_sync_state_to_kernel(synced, offset);
                last_kernel_synced = synced;
            }
        }
        descriptor_update_clock_source();

        acmp_tick(sock, now);
        avdecc_peers_poll(sock, now);
        avdecc_probe_poll(sock, now);
        msrp_listener_follow(listener_ctx, g_num_streams);
        publish_listener_identities_to_kernel();

        for (int k = 0; k < g_num_streams; k++) {
            msrp_talker_tick(&talker_ctx[k], &g_status, &g_status_lock);
            msrp_listener_tick(&listener_ctx[k], &g_status, &g_status_lock);
        }

        unsigned long long cbs_bps = 0;
        {
            FILE *f = fopen("/proc/snd_igb_avb/tx_engine", "r");
            if (f) {
                char line[64];
                int tx_timer_active = 0;
                unsigned long long ticks = 0;
                unsigned long long late_launch = 0;
                unsigned long long xrun = 0;
                unsigned long long gated_ticks = 0;
                unsigned long long silence_packets = 0;
                int gated = 1;
                while (fgets(line, sizeof(line), f)) {
                    int active;
                    if (sscanf(line, "tx_timer_active=%d", &active) == 1)
                        tx_timer_active = active;
                    else if (sscanf(line, "tx_engine_ticks=%llu", &ticks) == 1)
                        ;
                    else if (sscanf(line, "tx_silence_packets=%llu", &silence_packets) == 1)
                        ;
                    else if (sscanf(line, "tx_late_launch=%llu", &late_launch) == 1)
                        ;
                    else if (sscanf(line, "tx_xrun=%llu", &xrun) == 1)
                        ;
                    else if (sscanf(line, "tx_gated=%d", &gated) == 1)
                        ;
                    else if (sscanf(line, "tx_gated_ticks=%llu", &gated_ticks) == 1)
                        ;
                    else if (sscanf(line, "aggregate_idleslope_bps=%llu", &cbs_bps) == 1)
                        ;
                }
                fclose(f);
                uint32_t ticks_per_s = 0;
                int have_rate = last_tx_ms && now > last_tx_ms && ticks >= last_tx_ticks;
                if (have_rate)
                    ticks_per_s = (uint32_t)((ticks - last_tx_ticks) * 1000ULL /
                                             (now - last_tx_ms));
                {
                    static struct avb_diag tx_stall;
                    char body[160];
                    snprintf(body, sizeof body,
                             "AVTP talker: TX engine armed but launching no packets: "
                             "restart the stack [%016llx]", (unsigned long long)ENTITY_ID);
                    avb_diag_update(&tx_stall, have_rate && tx_timer_active && !gated && !ticks_per_s &&
                                    gptp_link_carrier(),
                                    now, 3000, body);
                }
                last_tx_ticks = ticks;
                last_tx_ms = now;
                status_update_tx_engine(tx_timer_active, ticks_per_s,
                                        late_launch, xrun, gated, gated_ticks,
                                        silence_packets);
            }
        }

        {
            int match = cbs_bps ? (cbs_bps == msrp_reserved_bps) : -1;

            if (match != last_bw_match) {
                if (match == 0)
                    avb_log(L_SYS, LOG_WARNING, "reserved BW %llu != CBS idleSlope %llu bps",
                            (unsigned long long)msrp_reserved_bps, cbs_bps);
                else if (match == 1)
                    avb_log(L_SYS, LOG_INFO, "reserved BW == CBS idleSlope (%llu bps, %d streams)",
                            cbs_bps, g_num_streams);
                last_bw_match = match;
            }
            status_update_cbs_bw(cbs_bps, msrp_reserved_bps, match);
        }

         {
             static unsigned long long last_cap_frames;
             FILE *f = fopen("/proc/snd_igb_avb/rx_engine", "r");

             status_update_media_clk(descriptor_get_clock_source_index() ==
                                     CLOCK_SOURCE_INDEX_INTERNAL);
             if (f) {
                 char line[192];
                 long long drift = 0;
                 int warn = 0;
                 unsigned long long slips = 0, seq_err = 0, cap_frames = 0, unmatched = 0;
                 int capture_running;

                 while (fgets(line, sizeof(line), f)) {
                     int k, ready;
                     long long sliding;

                     sscanf(line, "rx_media_drift_ppb=%lld", &drift);
                     sscanf(line, "rx_drift_warn_ppb=%d", &warn);
                     sscanf(line, "rx_clock_slips=%llu", &slips);
                     sscanf(line, "rx_seq_errors=%llu", &seq_err);
                     sscanf(line, "capture_frames_total=%llu", &cap_frames);
                     sscanf(line, "rx_unmatched=%llu", &unmatched);
                     if (sscanf(line, "stream[%d]: rx_pkts=%*u drift_ppb=%*d sliding_ppb=%lld window_ready=%d",
                                &k, &sliding, &ready) == 3)
                         status_update_rx_stream_clock(k, sliding, ready);
                 }
                 fclose(f);
                 capture_running = cap_frames != last_cap_frames;
                 last_cap_frames = cap_frames;
                 status_update_rx_engine(1, drift, warn, slips, seq_err, unmatched, capture_running);
             } else {
                 status_update_rx_engine(0, 0, 0, 0, 0, 0, 0);
                 for (int k = 0; k < g_num_streams; k++)
                     status_update_rx_stream_clock(k, 0, 0);
             }
         }

      if (now - last_render >= 500) {
         status_update_filter_ids();
         status_render(argv[1], start_ms);
         last_render = now;
      }

      usleep(100 * 1000);
   }

   status_screen_teardown();
   avb_log(L_SYS, LOG_INFO, "shutting down: ADP departing, leaving streams");

   avdecc_adp_send_departing(sock);

   /* join the control thread before closing the socket it reads from */
   pthread_join(control_thread_id, NULL);
   avtp_socket_close();

   for (int k = 0; k < g_num_streams; k++) {
       mrp_unadvertise_stream(talker_ctx[k].ox_stream, talker_ctx[k].ox_mac,
                              talker_ctx[k].pktsz, talker_ctx[k].interval,
                              talker_ctx[k].latency, talker_ctx[k].vid,
                              talker_ctx[k].priority);
   }
   msrp_listener_leave_all(listener_ctx, g_num_streams);

   msrp_listener_cleanup();
   msrp_talker_cleanup();
   msrp_domain_cleanup();
   avdecc_adp_cleanup();
   gptp_link_cleanup();
   gptp_client_cleanup();
   avb_log_close();

   if (g_maap_ctx.count > 0) {
       avb_log(L_SYS, LOG_INFO, "releasing MAAP block base=0x%012llx count=%u",
               (unsigned long long)g_maap_ctx.base, g_maap_ctx.count);
       maap_release_adresses(&g_maap_ctx);
       g_maap_ctx.count = 0;
   }

   return 0;
}