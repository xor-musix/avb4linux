// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#ifndef AVB_STATUS_H
#define AVB_STATUS_H

#include <stdint.h>
#include <pthread.h>

#include "avb_status_types.h"
#include "stream_identity.h"

#define STATUS_TX_PEERS 8

struct avb_stream {
    uint64_t stream_id;
    uint8_t dest_mac[6];
    uint16_t vlan;
    uint8_t sequence;
    uint8_t dbc;
    uint64_t format;
    uint8_t channels;
    uint16_t samplerate;
    int rx_filter_id;
    int align_offset_frames;
};

struct avb_status {
   int       gptp_up;
   int       gptp_locked;
   int       gptp_synced;
   uint64_t  gptp_gm;
   int64_t   gptp_offset_ns;
   uint8_t   gptp_domain;

   int64_t   gptp_offset_min_ns;
   int64_t   gptp_offset_max_ns;
   int64_t   gptp_offset_sum_ns;
   uint32_t  gptp_offset_samples;

   struct {
      enum talker_state state;
      int  attempts;
      int  listeners;
   } talker[MAX_STREAMS];
   struct {
      enum listener_rsv state;
      int  attempts;
   } listener[MAX_STREAMS];

   int       domain_ok, domain_class, domain_prio, domain_vid;

   int       controllers_seen;
   uint64_t  last_controller;
   uint64_t  last_adp_ms;

   uint64_t  maap_base;
   int       maap_lvl;
   int       maap_count;

   uint64_t  bw_reserved_bps;
   uint64_t  bw_cbs_programmed_bps;
   int       bw_match;

     struct {
        int      tx_bound, rx_bound;
        uint64_t tx_peer, rx_peer;
        int      tx_n;
        uint64_t tx_peers[STATUS_TX_PEERS];
        uint64_t rx_sid;
     } conn[MAX_STREAMS];

      int tx_timer_active;
      uint32_t tx_ticks_per_s;
      uint64_t tx_late_launch;
      uint64_t tx_xrun;
      int tx_gated;
      uint64_t tx_gated_ticks;
      uint64_t tx_silence_packets;

       int      media_clk_internal;
       int      rx_engine_seen;
       long long rx_drift_ppb;
       long long rx_drift_ppb_sliding;
       int      rx_drift_stream;
       int      rx_drift_warn_ppb;
       uint64_t rx_clock_slips;
       uint64_t rx_seq_errors;
       uint64_t rx_unmatched;
       int      rx_sliding_ready;
       int      rx_capture_running;

     uint32_t available_index;

       struct avb_status_stream {
          uint64_t stream_id; uint8_t dest_mac[6]; uint8_t dir;
          int      rx_filter_id;
          uint8_t  rx_src_mac[6], rx_dst_mac[6];
          uint64_t rx_pkts, rx_drops, rx_late;
          uint32_t rx_pkts_per_s;
          int      align_offset_frames;
          long long rx_drift_ppb_sliding;
          int      rx_window_ready;
       } streams[MAX_STREAMS];
      int n_streams;
  };

#define MAAP_LVL_FIXED    0
#define MAAP_LVL_FALLBACK 1
#define MAAP_LVL_DYNAMIC  2

void gptp_poll(struct avb_status *status, pthread_mutex_t *status_lock,
               int *was_locked, int *was_synced, uint64_t *was_gm);

void status_init(void);
void status_render(const char *iface, uint64_t start_ms);
void status_update_gptp(int up, int locked, int synced, uint64_t gm, int64_t offset, uint8_t domain);
void status_update_talker(int k, enum talker_state state, int attempts, int listeners);
void status_update_listener(int k, enum listener_rsv state, int attempts);
void status_update_domain(int ok, int class, int prio, int vid);
void status_update_controller(uint64_t controller_id, uint64_t adp_ms);
void status_update_maap(uint64_t base, int level, int count);
void status_update_tx_engine(int timer_active, uint32_t ticks_per_s,
                             uint64_t late_launch, uint64_t xrun,
                             int gated, uint64_t gated_ticks,
                             uint64_t silence_packets);
void status_update_cbs_bw(uint64_t cbs_bps, uint64_t reserved_bps, int match);
void status_update_rx_engine(int seen, long long drift_ppb, int warn_ppb,
                             uint64_t clock_slips, uint64_t seq_errors,
                             uint64_t unmatched, int capture_running);
void status_update_rx_stream_clock(int k, long long sliding_ppb, int window_ready);
void status_update_media_clk(int internal);
void status_update_conns(int idx, int tx_bound, uint64_t tx_peer, int tx_n, const uint64_t *tx_peers,
                         int rx_bound, uint64_t rx_peer, uint64_t rx_sid);
void status_update_available_index(uint32_t idx);
void status_update_streams(int n, struct avb_stream *streams_out, struct avb_stream *streams_in);
void status_update_filter_ids(void);
void status_screen_teardown(void);

extern uint32_t samplerate;

extern struct avb_status g_status;
extern pthread_mutex_t g_status_lock;

#endif