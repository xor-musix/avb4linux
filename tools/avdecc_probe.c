// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <jdksavdecc.h>

#include "avb_log.h"
#include "utils.h"
#include "stream_identity.h"
#include "avdecc_peers.h"
#include "avdecc_probe.h"

extern int sendMsg(int sock, char *buffer, int buflen);

extern char MAC[6];
extern char DMAC[];
extern uint64_t CONTROLLER_ID;
extern uint64_t ENTITY_ID;
extern uint32_t samplerate;

#define HEADER_OFFSET 14
#define MACLEN 6
#define FRAME_MAX 96

#define MAX_PEERS      AVDECC_PEERS_MAX
#define PROBE_STREAMS  32
#define PROBE_CLOCKS   16
#define PROBE_DIAGS    8
#define CMD_TIMEOUT_MS 250
#define IN_PROGRESS_MS 1000
#define REFRESH_MS     60000
#define ANOMALY_REFRESH_MS 5000
#define KICK_GAP_MS    2000
#define STUCK_MS       10000
#define SILENT_AFTER   3

#define F_CONNECTED   JDKSAVDECC_AEM_COMMAND_SET_STREAM_INFO_FLAG_CONNECTED
#define F_MSRP_FAIL   JDKSAVDECC_AEM_COMMAND_SET_STREAM_INFO_FLAG_MSRP_FAILURE_VALID
#define F_SID_VALID   JDKSAVDECC_AEM_COMMAND_SET_STREAM_INFO_FLAG_STREAM_ID_VALID
#define F_SAVED_STATE JDKSAVDECC_AEM_COMMAND_SET_STREAM_INFO_FLAG_SAVED_STATE
#define F_FAST_CONN   JDKSAVDECC_AEM_COMMAND_SET_STREAM_INFO_FLAG_FAST_CONNECT

#define CNT_LOCKED   0x00000001u
#define CNT_UNLOCKED 0x00000002u

#define ACMP_STATUS_NOT_SUPPORTED 31

enum { PC_REGISTER, PC_RATE, PC_CLKSRC, PC_CLKDOM, PC_CLKDESC, PC_COUNTERS, PC_INFO_IN, PC_INFO_OUT,
       PC_FMT_IN, PC_FMT_OUT, PC_RXSTATE, PC_N };

static const char *const pc_name[PC_N] = {
   "REGISTER_UNSOLICITED_NOTIFICATION", "GET_SAMPLING_RATE", "GET_CLOCK_SOURCE", "READ_DESCRIPTOR(CLOCK_DOMAIN)", "READ_DESCRIPTOR(CLOCK_SOURCE)",
   "GET_COUNTERS",
   "GET_STREAM_INFO(in)", "GET_STREAM_INFO(out)", "GET_STREAM_FORMAT(in)", "GET_STREAM_FORMAT(out)",
   "ACMP GET_RX_STATE",
};

#define ST_NONE    -1
#define ST_TIMEOUT -2

struct pstream {
   int      known;
   uint32_t flags;
   uint64_t format, sid, bridge, talker;
   uint16_t talker_uid;
   uint8_t  fail_code;
   uint64_t want_since_ms;
   uint64_t ctx_ms;
   int      ctx_k;
};

struct pdiag {
   int      kind, severe, shown, seen, first_sweep;
   char     scope[64], cause[128];
};

struct ppeer {
   uint64_t eid;
   uint8_t  mac[MACLEN];
   int      present;
   uint32_t avail;
   uint16_t n_in, n_out;
   int      kicked, sweeping, step, j, ph, misses, sweeps, silent_logged, unsolicited_logged;
   uint64_t next_ms, sweep_start_ms;
   int      inflight, cmd, idx, retried;
   uint16_t seq;
   uint64_t deadline_ms;
   int      unsup[PC_N], sw_st[PC_N], st[PC_N], st_logged[PC_N], logged_once;
   int      rate_ok, clk_ok, cnt_ok, unlocked;
   uint32_t rate, cnt_locked, cnt_unlocked;
   uint16_t clk_idx;
   struct { int known; uint16_t type, loc_type, loc_index; } clk[PROBE_CLOCKS];
   struct pstream in[PROBE_STREAMS], out[PROBE_STREAMS];
   struct pdiag diag[PROBE_DIAGS];
   int      n_diag;
};

static struct ppeer g_probe[MAX_PEERS];
static int g_n_probe;
static uint16_t g_seq;
static pthread_mutex_t g_probe_lock = PTHREAD_MUTEX_INITIALIZER;

static const uint32_t sfc_rate[8] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000, 0 };
static const uint32_t nsr_rate[16] = { 0, 8000, 16000, 32000, 44100, 48000, 88200, 96000,
                                       176400, 192000, 24000 };

int avdecc_format_decode(uint64_t fmt, uint32_t *rate, int *channels)
{
   unsigned subtype = (fmt >> 56) & 0x7f;

   if (fmt >> 63)
      return 0;
   if (subtype == 0x00) {
      unsigned sf = (fmt >> 55) & 1, f = (fmt >> 49) & 0x3f;
      unsigned evt = (fmt >> 43) & 0x1f, sfc = (fmt >> 40) & 7, dbs = (fmt >> 32) & 0xff;
      unsigned labels = ((fmt >> 16) & 0xff) + ((fmt >> 8) & 0xff);
      if (!sf || f != 0x10 || evt || !sfc_rate[sfc])
         return 0;
      *rate = sfc_rate[sfc];
      *channels = labels ? (int)labels : (int)dbs;
      return 1;
   }
   if (subtype == 0x02) {
      unsigned nsr = (fmt >> 48) & 0xf;
      if (!nsr_rate[nsr])
         return 0;
      *rate = nsr_rate[nsr];
      *channels = (fmt >> 22) & 0x3ff;
      return 1;
   }
   return 0;
}

static void fmt_rate(uint32_t rate, char *buf, size_t n)
{
   if (rate % 1000)
      snprintf(buf, n, "%u.%u kHz", rate / 1000, rate % 1000 / 100);
   else
      snprintf(buf, n, "%u kHz", rate / 1000);
}

void avdecc_format_str(uint64_t fmt, char *buf, size_t n)
{
   uint32_t rate;
   int ch;
   char r[16];

   if (!avdecc_format_decode(fmt, &rate, &ch)) {
      snprintf(buf, n, "0x%016llx", (unsigned long long)fmt);
      return;
   }
   fmt_rate(rate, r, sizeof r);
   snprintf(buf, n, "%s%s/%dch", ((fmt >> 56) & 0x7f) == 0x02 ? "AAF " : "", r, ch);
}

static const char *msrp_failure_name(unsigned code)
{
   static const char *const names[] = {
      "", "insufficient bandwidth", "insufficient bridge resources",
      "insufficient bandwidth for traffic class", "stream id in use by another talker",
      "destination address already in use", "pre-empted by higher rank", "reported latency changed",
      "egress port not AVB capable", "use a different destination address", "out of MSRP resources",
      "out of MMRP resources", "cannot store destination address", "priority is not an SR class",
      "max frame size too large", "max fan-in ports reached", "first value changed for stream id",
      "VLAN blocked on egress port", "VLAN tagging disabled on egress port", "SR class priority mismatch",
   };
   return code < sizeof names / sizeof names[0] ? names[code] : "unknown";
}

static const char *status_name(int st, char *buf, size_t n)
{
   switch (st) {
   case ST_TIMEOUT:                              return "no answer";
   case JDKSAVDECC_AEM_STATUS_SUCCESS:           return "ok";
   case JDKSAVDECC_AEM_STATUS_NOT_IMPLEMENTED:   return "not implemented";
   case JDKSAVDECC_AEM_STATUS_NO_SUCH_DESCRIPTOR: return "no such descriptor";
   case JDKSAVDECC_AEM_STATUS_BAD_ARGUMENTS:     return "bad arguments";
   case JDKSAVDECC_AEM_STATUS_NOT_SUPPORTED:     return "not supported";
   default: snprintf(buf, n, "status %d", st);  return buf;
   }
}

static struct ppeer *probe_find(uint64_t eid)
{
   for (int i = 0; i < g_n_probe; i++)
      if (g_probe[i].eid == eid)
         return &g_probe[i];
   return NULL;
}

static const char *peer_name(const struct ppeer *p, char *buf, size_t n)
{
   if (!avdecc_peers_name(p->eid, buf, n))
      snprintf(buf, n, "%016llx", (unsigned long long)p->eid);
   return buf;
}

static uint64_t our_mac(void)
{
   uint64_t m = 0;
   for (int i = 0; i < MACLEN; i++)
      m = m << 8 | (uint8_t)MAC[i];
   return m;
}

static void ranges(uint64_t bits, int base, char *buf, size_t n)
{
   size_t o = 0;

   buf[0] = '\0';
   for (int i = 0; i < 64 && o < n; i++) {
      if (!(bits >> i & 1))
         continue;
      int e = i;
      while (e + 1 < 64 && bits >> (e + 1) & 1)
         e++;
      o += snprintf(buf + o, n - o, e > i ? "%s%d-%d" : "%s%d", o ? "," : "", i + base, e + base);
      i = e;
   }
}

static int aem_frame(char *f, const struct ppeer *p, uint16_t type, uint16_t dtype, uint16_t didx)
{
   int read = type == JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR;
   int bare = type == JDKSAVDECC_AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION;
   int len = read ? JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR_COMMAND_LEN
           : bare ? JDKSAVDECC_AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION_COMMAND_LEN
                  : JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO_COMMAND_LEN;
   struct jdksavdecc_aecpdu_aem h;
   int o = HEADER_OFFSET + JDKSAVDECC_AECPDU_AEM_LEN;

   memset(f, 0, HEADER_OFFSET + len);
   memset(&h, 0, sizeof h);
   h.aecpdu_header.header.cd = 1;
   h.aecpdu_header.header.subtype = JDKSAVDECC_SUBTYPE_AECP;
   h.aecpdu_header.header.message_type = JDKSAVDECC_AECP_MESSAGE_TYPE_AEM_COMMAND;
   h.aecpdu_header.header.status = JDKSAVDECC_AEM_STATUS_SUCCESS;
   h.aecpdu_header.header.control_data_length = len - JDKSAVDECC_COMMON_CONTROL_HEADER_LEN;
   jdksavdecc_eui64_init_from_uint64(&h.aecpdu_header.header.target_entity_id, p->eid);
   jdksavdecc_eui64_init_from_uint64(&h.aecpdu_header.controller_entity_id, CONTROLLER_ID);
   h.aecpdu_header.sequence_id = p->seq;
   h.command_type = type;

   memcpy(f, p->mac, MACLEN);
   memcpy(&f[MACLEN], MAC, MACLEN);
   f[12] = 0x22; f[13] = 0xf0;
   jdksavdecc_aecpdu_aem_write(&h, f, HEADER_OFFSET, HEADER_OFFSET + len);
   if (bare)
      return HEADER_OFFSET + len;
   if (read)
      o += 4;
   jdksavdecc_uint16_set(dtype, f, o);
   jdksavdecc_uint16_set(didx, f, o + 2);
   return HEADER_OFFSET + len;
}

static int acmp_frame(char *f, const struct ppeer *p, uint16_t uid)
{
   struct jdksavdecc_acmpdu a;

   memset(f, 0, HEADER_OFFSET + JDKSAVDECC_ACMPDU_LEN);
   memset(&a, 0, sizeof a);
   a.header.cd = 1;
   a.header.subtype = JDKSAVDECC_SUBTYPE_ACMP;
   a.header.message_type = JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND;
   a.header.status = JDKSAVDECC_ACMP_STATUS_SUCCESS;
   a.header.control_data_length = JDKSAVDECC_ACMPDU_LEN - JDKSAVDECC_COMMON_CONTROL_HEADER_LEN;
   jdksavdecc_eui64_init_from_uint64(&a.controller_entity_id, CONTROLLER_ID);
   jdksavdecc_eui64_init_from_uint64(&a.listener_entity_id, p->eid);
   a.listener_unique_id = uid;
   a.sequence_id = p->seq;

   memcpy(f, DMAC, MACLEN);
   memcpy(&f[MACLEN], MAC, MACLEN);
   f[12] = 0x22; f[13] = 0xf0;
   jdksavdecc_acmpdu_write(&a, f, HEADER_OFFSET, HEADER_OFFSET + JDKSAVDECC_ACMPDU_LEN);
   return HEADER_OFFSET + JDKSAVDECC_ACMPDU_LEN;
}

static int cmd_frame(char *f, const struct ppeer *p)
{
   switch (p->cmd) {
   case PC_REGISTER:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, 0, 0);
   case PC_RATE:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_GET_SAMPLING_RATE, JDKSAVDECC_DESCRIPTOR_AUDIO_UNIT, 0);
   case PC_CLKSRC:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_GET_CLOCK_SOURCE, JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN, 0);
   case PC_CLKDOM:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR, JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN, 0);
   case PC_CLKDESC:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR, JDKSAVDECC_DESCRIPTOR_CLOCK_SOURCE, p->idx);
   case PC_COUNTERS:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_GET_COUNTERS, JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN, 0);
   case PC_INFO_IN:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO, JDKSAVDECC_DESCRIPTOR_STREAM_INPUT, p->idx);
   case PC_INFO_OUT:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO, JDKSAVDECC_DESCRIPTOR_STREAM_OUTPUT, p->idx);
   case PC_FMT_IN:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT, JDKSAVDECC_DESCRIPTOR_STREAM_INPUT, p->idx);
   case PC_FMT_OUT:
      return aem_frame(f, p, JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT, JDKSAVDECC_DESCRIPTOR_STREAM_OUTPUT, p->idx);
   default:
      return acmp_frame(f, p, p->idx);
   }
}

static int next_cmd(struct ppeer *p, int *cmd, int *idx)
{
   for (;;) {
      switch (p->step) {
      case 0:
         p->step++;
         if (!p->unsup[PC_REGISTER]) { *cmd = PC_REGISTER; *idx = 0; return 1; }
         break;
      case 1:
         p->step++;
         if (!p->unsup[PC_RATE]) { *cmd = PC_RATE; *idx = 0; return 1; }
         break;
      case 2:
         p->step++;
         if (!p->unsup[PC_CLKSRC]) { *cmd = PC_CLKSRC; *idx = 0; return 1; }
         if (!p->unsup[PC_CLKDOM]) { *cmd = PC_CLKDOM; *idx = 0; return 1; }
         break;
      case 3:
         p->step++;
         if (!p->unsup[PC_CLKDESC] && p->clk_ok && p->clk_idx < PROBE_CLOCKS && !p->clk[p->clk_idx].known) {
            *cmd = PC_CLKDESC; *idx = p->clk_idx; return 1;
         }
         break;
      case 4:
         p->step++;
         if (!p->unsup[PC_COUNTERS]) { *cmd = PC_COUNTERS; *idx = 0; return 1; }
         break;
      case 5:
      case 6: {
         int in = p->step == 5, n = in ? p->n_in : p->n_out;
         int info = in ? PC_INFO_IN : PC_INFO_OUT, fmt = in ? PC_FMT_IN : PC_FMT_OUT;
         if (p->j >= n) {
            p->step++;
            p->j = p->ph = 0;
            break;
         }
         *idx = p->j;
         if (!p->unsup[info]) { *cmd = info; p->j++; return 1; }
         if (p->ph == 0) {
            p->ph = 1;
            if (!p->unsup[fmt]) { *cmd = fmt; return 1; }
         }
         p->ph = 0;
         p->j++;
         if (in && !p->unsup[PC_RXSTATE]) { *cmd = PC_RXSTATE; return 1; }
         break;
      }
      default:
         return 0;
      }
   }
}

static void note_status(struct ppeer *p, int cmd, int st)
{
   if (st == JDKSAVDECC_AEM_STATUS_SUCCESS || p->sw_st[cmd] != JDKSAVDECC_AEM_STATUS_SUCCESS)
      if (st != ST_TIMEOUT || p->sw_st[cmd] == ST_NONE)
         p->sw_st[cmd] = st;
}

static void log_stream(const struct ppeer *p, int j, const struct pstream *old, const struct pstream *s)
{
   char name[65], f[32], sid[40] = "";

   char talker[48] = "";

   if (old->known && old->flags == s->flags && old->sid == s->sid && old->format == s->format &&
       old->talker == s->talker && old->talker_uid == s->talker_uid)
      return;
   if (s->flags & F_SID_VALID)
      snprintf(sid, sizeof sid, ", sid %016llx", (unsigned long long)s->sid);
   if (s->talker)
      snprintf(talker, sizeof talker, ", talker %016llx:%u", (unsigned long long)s->talker, s->talker_uid);
   avdecc_format_str(s->format, f, sizeof f);
   avb_log(L_AVDECC, LOG_INFO, "probe %s: in %d %s%s%s%s%s, %s, flags 0x%08x", peer_name(p, name, sizeof name),
           j + 1, s->flags & F_CONNECTED ? "connected" : "not connected", sid, talker,
           s->flags & F_SAVED_STATE ? " (saved state)" : "", s->flags & F_FAST_CONN ? " (fast connect)" : "",
           f, s->flags);
}

static void store_result(struct ppeer *p, uint8_t status, const char *f, int len)
{
   const int b = HEADER_OFFSET;

   if (p->cmd == PC_CLKSRC && status != JDKSAVDECC_AEM_STATUS_SUCCESS) {
      p->unsup[PC_CLKSRC] = 1;
      p->step = 2;
      return;
   }
   if (status == JDKSAVDECC_AEM_STATUS_NOT_IMPLEMENTED || status == JDKSAVDECC_AEM_STATUS_NOT_SUPPORTED) {
      p->unsup[p->cmd] = 1;
      if (p->cmd == PC_INFO_IN || p->cmd == PC_INFO_OUT) {
         p->j = p->idx;
         p->ph = 0;
      }
      return;
   }
   if (status != JDKSAVDECC_AEM_STATUS_SUCCESS) {
      if (p->cmd == PC_CLKDESC && p->idx < PROBE_CLOCKS) {
         p->clk[p->idx].known = 1;
         p->clk[p->idx].type = 0xffff;
      }
      return;
   }

   switch (p->cmd) {
   case PC_RATE:
      if (len >= b + JDKSAVDECC_AEM_COMMAND_GET_SAMPLING_RATE_RESPONSE_LEN) {
         p->rate = jdksavdecc_uint32_get(f, b + JDKSAVDECC_AEM_COMMAND_GET_SAMPLING_RATE_RESPONSE_OFFSET_SAMPLING_RATE);
         p->rate_ok = 1;
      }
      break;
   case PC_CLKSRC:
      if (len >= b + JDKSAVDECC_AEM_COMMAND_GET_CLOCK_SOURCE_RESPONSE_LEN) {
         p->clk_idx = jdksavdecc_uint16_get(f, b + JDKSAVDECC_AEM_COMMAND_GET_CLOCK_SOURCE_RESPONSE_OFFSET_CLOCK_SOURCE_INDEX);
         p->clk_ok = 1;
      }
      break;
   case PC_CLKDOM: {
      int d = b + JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR_RESPONSE_OFFSET_DESCRIPTOR;
      if (len >= d + JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN_OFFSET_CLOCK_SOURCE_INDEX + 2) {
         p->clk_idx = jdksavdecc_uint16_get(f, d + JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN_OFFSET_CLOCK_SOURCE_INDEX);
         p->clk_ok = 1;
      }
      break;
   }
   case PC_CLKDESC: {
      int d = b + JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR_RESPONSE_OFFSET_DESCRIPTOR;
      if (len < d + JDKSAVDECC_DESCRIPTOR_CLOCK_SOURCE_OFFSET_CLOCK_SOURCE_LOCATION_INDEX + 2 || p->idx >= PROBE_CLOCKS)
         break;
      p->clk[p->idx].known     = 1;
      p->clk[p->idx].type      = jdksavdecc_descriptor_clock_source_get_clock_source_type(f, d);
      p->clk[p->idx].loc_type  = jdksavdecc_descriptor_clock_source_get_clock_source_location_type(f, d);
      p->clk[p->idx].loc_index = jdksavdecc_descriptor_clock_source_get_clock_source_location_index(f, d);
      break;
   }
   case PC_COUNTERS:
      if (len >= b + JDKSAVDECC_AEM_COMMAND_GET_COUNTERS_RESPONSE_OFFSET_COUNTERS_BLOCK + 8) {
         uint32_t valid = jdksavdecc_uint32_get(f, b + JDKSAVDECC_AEM_COMMAND_GET_COUNTERS_RESPONSE_OFFSET_COUNTERS_VALID);
         int blk = b + JDKSAVDECC_AEM_COMMAND_GET_COUNTERS_RESPONSE_OFFSET_COUNTERS_BLOCK;
         uint32_t l = jdksavdecc_uint32_get(f, blk), u = jdksavdecc_uint32_get(f, blk + 4);
         if ((valid & (CNT_LOCKED | CNT_UNLOCKED)) != (CNT_LOCKED | CNT_UNLOCKED)) {
            p->cnt_ok = p->unlocked = 0;
            break;
         }
         if (p->cnt_ok && u - p->cnt_unlocked != l - p->cnt_locked)
            p->unlocked = u - p->cnt_unlocked > l - p->cnt_locked;
         p->cnt_ok = 1;
         p->cnt_locked = l;
         p->cnt_unlocked = u;
      }
      break;
   case PC_INFO_IN:
   case PC_INFO_OUT:
      if (len >= b + JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO_RESPONSE_LEN && p->idx < PROBE_STREAMS) {
         struct pstream *s = p->cmd == PC_INFO_IN ? &p->in[p->idx] : &p->out[p->idx], old = *s;
         struct jdksavdecc_eui64 v;
         s->flags = jdksavdecc_uint32_get(f, b + JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO_RESPONSE_OFFSET_AEM_STREAM_INFO_FLAGS);
         v = jdksavdecc_eui64_get(f, b + JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO_RESPONSE_OFFSET_STREAM_FORMAT);
         s->format = jdksavdecc_eui64_convert_to_uint64(&v);
         v = jdksavdecc_eui64_get(f, b + JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO_RESPONSE_OFFSET_STREAM_ID);
         s->sid = jdksavdecc_eui64_convert_to_uint64(&v);
         s->fail_code = jdksavdecc_uint8_get(f, b + JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO_RESPONSE_OFFSET_MSRP_FAILURE_CODE);
         v = jdksavdecc_eui64_get(f, b + JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO_RESPONSE_OFFSET_MSRP_FAILURE_BRIDGE_ID);
         s->bridge = jdksavdecc_eui64_convert_to_uint64(&v);
         s->known = 1;
         if (p->cmd == PC_INFO_IN)
            log_stream(p, p->idx, &old, s);
      }
      break;
   case PC_FMT_IN:
   case PC_FMT_OUT:
      if (len >= b + JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT_RESPONSE_LEN && p->idx < PROBE_STREAMS) {
         struct pstream *s = p->cmd == PC_FMT_IN ? &p->in[p->idx] : &p->out[p->idx];
         struct jdksavdecc_eui64 v = jdksavdecc_eui64_get(f, b + JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT_RESPONSE_OFFSET_STREAM_FORMAT);
         s->format = jdksavdecc_eui64_convert_to_uint64(&v);
         if (p->cmd == PC_FMT_OUT)
            s->known = 1;
      }
      break;
   case PC_RXSTATE:
      if (len >= b + JDKSAVDECC_ACMPDU_LEN && p->idx < PROBE_STREAMS) {
         struct pstream *s = &p->in[p->idx], old = *s;
         struct jdksavdecc_eui64 v = jdksavdecc_common_control_header_get_stream_id(f, b);
         uint16_t aflags = jdksavdecc_acmpdu_get_flags(f, b);
         s->sid = jdksavdecc_eui64_convert_to_uint64(&v);
         v = jdksavdecc_acmpdu_get_talker_entity_id(f, b);
         s->talker = jdksavdecc_eui64_convert_to_uint64(&v);
         s->talker_uid = jdksavdecc_acmpdu_get_talker_unique_id(f, b);
         s->flags = (jdksavdecc_acmpdu_get_connection_count(f, b) ? F_CONNECTED : 0) |
                    (s->sid ? F_SID_VALID : 0) |
                    (aflags & JDKSAVDECC_ACMP_FLAG_SAVED_STATE ? F_SAVED_STATE : 0) |
                    (aflags & JDKSAVDECC_ACMP_FLAG_FAST_CONNECT ? F_FAST_CONN : 0);
         s->known = 1;
         log_stream(p, p->idx, &old, s);
      }
      break;
   }
}

struct cand { int kind, severe; char scope[64], cause[128]; uint64_t in, out, ks; };

static struct cand *cand_get(struct cand *c, int *n, int max, int kind, int severe, const char *cause)
{
   for (int i = 0; i < *n; i++)
      if (c[i].kind == kind && !strcmp(c[i].cause, cause))
         return &c[i];
   if (*n >= max)
      return NULL;
   memset(&c[*n], 0, sizeof c[*n]);
   c[*n].kind = kind;
   c[*n].severe = severe;
   snprintf(c[*n].cause, sizeof c[*n].cause, "%s", cause);
   return &c[(*n)++];
}

static void cand_scope(struct cand *c)
{
   char a[24], b[24];
   size_t o = 0;

   c->scope[0] = '\0';
   if (c->in) {
      ranges(c->in, 1, a, sizeof a);
      o += snprintf(c->scope + o, sizeof c->scope - o, "in %s", a);
   }
   if (c->ks) {
      ranges(c->ks, 0, b, sizeof b);
      o += snprintf(c->scope + o, sizeof c->scope - o, " → us:%s", b);
   }
   if (c->out && o < sizeof c->scope) {
      ranges(c->out, 1, a, sizeof a);
      snprintf(c->scope + o, sizeof c->scope - o, "%sout %s", o ? ", " : "", a);
   }
}

static int eval_peer(struct ppeer *p, uint64_t now, struct cand *c, int max)
{
   int n = 0;
   char t[128], r1[16], r2[16];
   uint64_t mac = our_mac();

   if (p->rate_ok && ((p->rate & 0x1fffffff) != samplerate || p->rate >> 29)) {
      snprintf(t, sizeof t, "sample rate %u%s (ours %u)", p->rate & 0x1fffffff, p->rate >> 29 ? " pulled" : "", samplerate);
      cand_get(c, &n, max, DIAG_RATE, 1, t);
   }

   fmt_rate(samplerate, r2, sizeof r2);
   for (int dir = 0; dir < 2; dir++) {
      int ns = dir ? p->n_out : p->n_in;
      for (int j = 0; j < ns && j < PROBE_STREAMS; j++) {
         struct pstream *s = dir ? &p->out[j] : &p->in[j];
         uint32_t rate;
         int ch;
         struct cand *x;
         if (!s->known)
            continue;
         if (s->format && avdecc_format_decode(s->format, &rate, &ch) &&
             (rate != samplerate || ch != CHANNELS_PER_STREAM)) {
            fmt_rate(rate, r1, sizeof r1);
            snprintf(t, sizeof t, "stream format %s/%dch (ours %s/%dch)", r1, ch, r2, CHANNELS_PER_STREAM);
            if ((x = cand_get(c, &n, max, DIAG_FORMAT, 1, t)))
               *(dir ? &x->out : &x->in) |= 1ULL << j;
         }
         if (s->flags & F_MSRP_FAIL) {
            snprintf(t, sizeof t, "MSRP failure %u %s @ %016llx", s->fail_code,
                     msrp_failure_name(s->fail_code), (unsigned long long)s->bridge);
            if ((x = cand_get(c, &n, max, DIAG_MSRP, 1, t)))
               *(dir ? &x->out : &x->in) |= 1ULL << j;
         }
      }
   }

   if (p->clk_ok && p->clk_idx < PROBE_CLOCKS && p->clk[p->clk_idx].known &&
       p->clk[p->clk_idx].type == JDKSAVDECC_CLOCK_SOURCE_TYPE_INPUT_STREAM &&
       p->clk[p->clk_idx].loc_type == JDKSAVDECC_DESCRIPTOR_STREAM_INPUT) {
      int j = p->clk[p->clk_idx].loc_index;
      if (j < p->n_in && j < PROBE_STREAMS && p->in[j].known && !(p->in[j].flags & F_CONNECTED)) {
         snprintf(t, sizeof t, "clock from in %d, not connected", j + 1);
         cand_get(c, &n, max, DIAG_CLOCK, 0, t);
      }
   }
   if (p->cnt_ok && p->unlocked)
      cand_get(c, &n, max, DIAG_CLOCK, 0, "clock unlocked (lost lock, not regained)");

   for (int j = 0; j < p->n_in && j < PROBE_STREAMS; j++) {
      struct pstream *s = &p->in[j];
      struct cand *x;
      int k = -1;

      if ((s->flags & F_SID_VALID) && s->sid >> 16 == mac)
         k = (int)(s->sid & 0xffff);
      else if (s->talker == ENTITY_ID)
         k = s->talker_uid;
      if (!s->known || (s->flags & F_CONNECTED) || k < 0) {
         s->want_since_ms = 0;
         continue;
      }
      if (k >= g_num_streams) {
         snprintf(t, sizeof t, "not connected: wants a stream we do not run (N=%d)", g_num_streams);
      } else {
         if (!s->want_since_ms)
            s->want_since_ms = now;
         if (now - s->want_since_ms < STUCK_MS) {
            if (p->next_ms > s->want_since_ms + STUCK_MS)
               p->next_ms = s->want_since_ms + STUCK_MS;
            continue;
         }
         if (s->ctx_ms >= s->want_since_ms && s->ctx_k == k)
            snprintf(t, sizeof t, "not connected although we answered its CONNECT_TX (MSRP? see journal)");
         else
            snprintf(t, sizeof t, "not connected, it sent no ACMP (listener stuck? power-cycle)");
      }
      if ((x = cand_get(c, &n, max, DIAG_STUCK, 1, t))) {
         x->in |= 1ULL << j;
         if (k < 64)
            x->ks |= 1ULL << k;
      }
   }

   for (int i = 0; i < n; i++)
      cand_scope(&c[i]);
   return n;
}

static void diag_body(const struct ppeer *p, const struct pdiag *d, char *buf, size_t n)
{
   char name[65];
   snprintf(buf, n, "%s%s%s: %s [%016llx]", peer_name(p, name, sizeof name), d->scope[0] ? " " : "",
            d->scope, d->cause, (unsigned long long)p->eid);
}

static void diag_drop(struct ppeer *p, int i)
{
   char body[320];

   if (p->diag[i].shown) {
      diag_body(p, &p->diag[i], body, sizeof body);
      avb_log(L_DIAG, LOG_INFO, "cleared: %s", body);
   }
   p->diag[i] = p->diag[--p->n_diag];
}

static void diag_merge(struct ppeer *p, const struct cand *c, int n)
{
   char body[320];

   for (int i = 0; i < p->n_diag; i++)
      p->diag[i].seen = 0;
   for (int k = 0; k < n; k++) {
      int i;
      for (i = 0; i < p->n_diag; i++)
         if (p->diag[i].kind == c[k].kind && !strcmp(p->diag[i].scope, c[k].scope) &&
             !strcmp(p->diag[i].cause, c[k].cause))
            break;
      if (i == p->n_diag) {
         if (p->n_diag == PROBE_DIAGS)
            continue;
         struct pdiag *d = &p->diag[p->n_diag++];
         memset(d, 0, sizeof *d);
         d->kind = c[k].kind;
         d->severe = c[k].severe;
         d->first_sweep = p->sweeps;
         snprintf(d->scope, sizeof d->scope, "%s", c[k].scope);
         snprintf(d->cause, sizeof d->cause, "%s", c[k].cause);
         p->kicked = 1;
      }
      p->diag[i].seen = 1;
      if (!p->diag[i].shown && p->sweeps > p->diag[i].first_sweep) {
         p->diag[i].shown = 1;
         diag_body(p, &p->diag[i], body, sizeof body);
         avb_log(L_DIAG, LOG_WARNING, "%s", body);
      }
   }
   for (int i = p->n_diag - 1; i >= 0; i--)
      if (!p->diag[i].seen)
         diag_drop(p, i);
}

static void log_statuses(struct ppeer *p)
{
   char line[480], name[65], sb[16];
   size_t o = 0;
   int changed = !p->logged_once, done[PC_N] = {0};

   for (int c = 0; c < PC_N; c++) {
      if (p->sw_st[c] != ST_NONE)
         p->st[c] = p->sw_st[c];
      if (p->st[c] != p->st_logged[c])
         changed = 1;
      p->st_logged[c] = p->st[c];
   }
   if (!changed)
      return;
   line[0] = '\0';
   for (int c = 0; c < PC_N && o < sizeof line; c++) {
      if (p->st[c] == ST_NONE || done[c])
         continue;
      o += snprintf(line + o, sizeof line - o, "%s%s: ", o ? "; " : "", status_name(p->st[c], sb, sizeof sb));
      for (int d = c, first = 1; d < PC_N && o < sizeof line; d++) {
         if (p->st[d] != p->st[c] || done[d])
            continue;
         done[d] = 1;
         o += snprintf(line + o, sizeof line - o, "%s%s", first ? "" : ", ", pc_name[d]);
         first = 0;
      }
   }
   p->logged_once = 1;
   avb_log(L_AVDECC, LOG_INFO, "probe %s: %s", peer_name(p, name, sizeof name), line);
}

static void finish_sweep(struct ppeer *p, uint64_t now)
{
   struct cand c[PROBE_DIAGS];

   p->sweeping = 0;
   p->sweeps++;
   p->silent_logged = 0;
   log_statuses(p);
   diag_merge(p, c, eval_peer(p, now, c, PROBE_DIAGS));
   if (p->n_diag && p->next_ms > now + ANOMALY_REFRESH_MS)
      p->next_ms = now + ANOMALY_REFRESH_MS;
}

static int send_current(struct ppeer *p, uint64_t now, char *buf)
{
   p->seq = ++g_seq;
   p->inflight = 1;
   p->deadline_ms = now + CMD_TIMEOUT_MS;
   return cmd_frame(buf, p);
}

static int start_next(struct ppeer *p, uint64_t now, char *buf)
{
   int cmd, idx;

   if (!next_cmd(p, &cmd, &idx)) {
      finish_sweep(p, now);
      return 0;
   }
   p->cmd = cmd;
   p->idx = idx;
   p->retried = 0;
   return send_current(p, now, buf);
}

static void peer_reset(struct ppeer *p)
{
   memset(p->unsup, 0, sizeof p->unsup);
   memset(p->clk, 0, sizeof p->clk);
   p->cnt_ok = p->unlocked = 0;
   p->logged_once = p->unsolicited_logged = 0;
   p->kicked = 1;
}

static void sync_peers(uint64_t now)
{
   static struct avdecc_peer_info li[AVDECC_PEERS_MAX];
   int n = avdecc_peers_list(li, AVDECC_PEERS_MAX);

   for (int i = 0; i < n; i++) {
      const struct avdecc_peer_info *d = &li[i];
      struct ppeer *p;

      if (!(d->entity_caps & JDKSAVDECC_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED) ||
          !(d->talker_sources || d->listener_sinks))
         continue;
      p = probe_find(d->eid);
      if (!p) {
         if (g_n_probe == MAX_PEERS)
            continue;
         p = &g_probe[g_n_probe++];
         memset(p, 0, sizeof *p);
         p->eid = d->eid;
         for (int c = 0; c < PC_N; c++)
            p->st[c] = p->st_logged[c] = ST_NONE;
      }
      int present = !d->departed && now - d->last_adp_ms <= d->valid_ms;
      if (present && (!p->present || d->available_index < p->avail))
         peer_reset(p);
      if (!present && p->present) {
         p->sweeping = p->inflight = 0;
         while (p->n_diag)
            diag_drop(p, p->n_diag - 1);
      }
      p->present = present;
      p->avail = d->available_index;
      memcpy(p->mac, d->mac, MACLEN);
      p->n_in  = d->listener_sinks < PROBE_STREAMS ? d->listener_sinks : PROBE_STREAMS;
      p->n_out = d->talker_sources < PROBE_STREAMS ? d->talker_sources : PROBE_STREAMS;
   }
}

void avdecc_probe_poll(int sock, uint64_t now)
{
   static char out[MAX_PEERS][FRAME_MAX];
   int len[MAX_PEERS], n_out = 0;
   char name[65];

   pthread_mutex_lock(&g_probe_lock);
   sync_peers(now);
   for (int i = 0; i < g_n_probe; i++) {
      struct ppeer *p = &g_probe[i];
      int l = 0;

      if (!p->present)
         continue;
      if (p->inflight) {
         if (now < p->deadline_ms)
            continue;
         if (!p->retried) {
            p->retried = 1;
            l = send_current(p, now, out[n_out]);
         } else {
            p->inflight = 0;
            note_status(p, p->cmd, ST_TIMEOUT);
            if (++p->misses >= SILENT_AFTER) {
               p->sweeping = 0;
               p->next_ms = now + REFRESH_MS;
               if (!p->silent_logged)
                  avb_log(L_AVDECC, LOG_INFO, "probe %s: no answer to %d commands in a row, next try in %ds",
                          peer_name(p, name, sizeof name), SILENT_AFTER, REFRESH_MS / 1000);
               p->silent_logged = 1;
               continue;
            }
            l = start_next(p, now, out[n_out]);
         }
      } else if (!p->sweeping &&
                 (now >= p->next_ms || (p->kicked && now - p->sweep_start_ms >= KICK_GAP_MS))) {
         p->sweeping = 1;
         p->kicked = 0;
         p->step = p->j = p->ph = p->misses = 0;
         p->sweep_start_ms = now;
         p->next_ms = now + REFRESH_MS;
         for (int c = 0; c < PC_N; c++)
            p->sw_st[c] = ST_NONE;
         l = start_next(p, now, out[n_out]);
      }
      if (l)
         len[n_out++] = l;
   }
   pthread_mutex_unlock(&g_probe_lock);

   for (int i = 0; i < n_out; i++)
      sendMsg(sock, out[i], len[i]);
}

static int expect_type(int cmd)
{
   switch (cmd) {
   case PC_REGISTER: return JDKSAVDECC_AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION;
   case PC_RATE:     return JDKSAVDECC_AEM_COMMAND_GET_SAMPLING_RATE;
   case PC_CLKSRC:   return JDKSAVDECC_AEM_COMMAND_GET_CLOCK_SOURCE;
   case PC_CLKDOM:
   case PC_CLKDESC:  return JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR;
   case PC_COUNTERS: return JDKSAVDECC_AEM_COMMAND_GET_COUNTERS;
   case PC_INFO_IN:
   case PC_INFO_OUT: return JDKSAVDECC_AEM_COMMAND_GET_STREAM_INFO;
   case PC_FMT_IN:
   case PC_FMT_OUT:  return JDKSAVDECC_AEM_COMMAND_GET_STREAM_FORMAT;
   default:          return -1;
   }
}

static int handle_answer(struct ppeer *p, int status, const char *frame, int len, char *buf)
{
   note_status(p, p->cmd, status);
   store_result(p, (uint8_t)status, frame, len);
   p->inflight = 0;
   p->misses = 0;
   return start_next(p, get_time_in_ms(), buf);
}

static void unsolicited(uint64_t eid, uint16_t type)
{
   char name[65];
   int first = 0;

   pthread_mutex_lock(&g_probe_lock);
   struct ppeer *p = probe_find(eid);
   if (p) {
      switch (type) {
      case JDKSAVDECC_AEM_COMMAND_SET_CONFIGURATION:
      case JDKSAVDECC_AEM_COMMAND_SET_STREAM_FORMAT:
      case JDKSAVDECC_AEM_COMMAND_SET_STREAM_INFO:
      case JDKSAVDECC_AEM_COMMAND_SET_SAMPLING_RATE:
      case JDKSAVDECC_AEM_COMMAND_SET_CLOCK_SOURCE:
         p->kicked = 1;
         break;
      }
      first = !p->unsolicited_logged;
      p->unsolicited_logged = 1;
   }
   if (first)
      avb_log(L_AVDECC, LOG_INFO, "probe %s: first unsolicited notification (command 0x%04x)",
              peer_name(p, name, sizeof name), type);
   pthread_mutex_unlock(&g_probe_lock);
}

void avdecc_probe_handle_aem_response(int sock, const char *frame, int len)
{
   const int b = HEADER_OFFSET;
   struct jdksavdecc_eui64 v;
   char buf[FRAME_MAX];
   int l;

   if (len < b + JDKSAVDECC_AECPDU_AEM_LEN)
      return;
   v = jdksavdecc_aecpdu_aem_get_controller_entity_id(frame, b);
   if (jdksavdecc_eui64_convert_to_uint64(&v) != CONTROLLER_ID)
      return;
   uint16_t type = jdksavdecc_aecpdu_aem_get_command_type(frame, b);
   v = jdksavdecc_common_control_header_get_stream_id(frame, b);
   uint64_t eid = jdksavdecc_eui64_convert_to_uint64(&v);
   if (type & 0x8000) {
      unsolicited(eid, type & 0x7fff);
      return;
   }
   uint16_t seq = jdksavdecc_aecpdu_aem_get_sequence_id(frame, b);
   uint8_t status = jdksavdecc_common_control_header_get_status(frame, b);

   pthread_mutex_lock(&g_probe_lock);
   struct ppeer *p = probe_find(eid);
   if (!p || !p->inflight || p->seq != seq || expect_type(p->cmd) != type) {
      pthread_mutex_unlock(&g_probe_lock);
      return;
   }
   if (type == JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR && status == JDKSAVDECC_AEM_STATUS_SUCCESS) {
      int d = b + JDKSAVDECC_AEM_COMMAND_READ_DESCRIPTOR_RESPONSE_OFFSET_DESCRIPTOR;
      uint16_t want = p->cmd == PC_CLKDOM ? JDKSAVDECC_DESCRIPTOR_CLOCK_DOMAIN : JDKSAVDECC_DESCRIPTOR_CLOCK_SOURCE;
      if (len < d + 4 || jdksavdecc_uint16_get(frame, d) != want ||
          jdksavdecc_uint16_get(frame, d + 2) != (p->cmd == PC_CLKDOM ? 0 : p->idx)) {
         pthread_mutex_unlock(&g_probe_lock);
         return;
      }
   }
   if (status == JDKSAVDECC_AEM_STATUS_IN_PROGRESS) {
      p->deadline_ms = get_time_in_ms() + IN_PROGRESS_MS;
      pthread_mutex_unlock(&g_probe_lock);
      return;
   }
   l = handle_answer(p, status, frame, len, buf);
   pthread_mutex_unlock(&g_probe_lock);
   if (l)
      sendMsg(sock, buf, l);
}

void avdecc_probe_handle_acmp_response(int sock, const char *frame, int len)
{
   const int b = HEADER_OFFSET;
   struct jdksavdecc_eui64 v;
   char buf[FRAME_MAX];
   int l;

   if (len < b + JDKSAVDECC_ACMPDU_LEN)
      return;
   if (jdksavdecc_common_control_header_get_control_data(frame, b) != JDKSAVDECC_ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE)
      return;
   v = jdksavdecc_acmpdu_get_controller_entity_id(frame, b);
   if (jdksavdecc_eui64_convert_to_uint64(&v) != CONTROLLER_ID)
      return;
   v = jdksavdecc_acmpdu_get_listener_entity_id(frame, b);
   uint64_t eid = jdksavdecc_eui64_convert_to_uint64(&v);
   uint16_t seq = jdksavdecc_acmpdu_get_sequence_id(frame, b);
   uint16_t uid = jdksavdecc_acmpdu_get_listener_unique_id(frame, b);
   uint8_t status = jdksavdecc_common_control_header_get_status(frame, b);

   pthread_mutex_lock(&g_probe_lock);
   struct ppeer *p = probe_find(eid);
   if (!p || !p->inflight || p->cmd != PC_RXSTATE || p->seq != seq || p->idx != uid) {
      pthread_mutex_unlock(&g_probe_lock);
      return;
   }
   l = handle_answer(p, status == ACMP_STATUS_NOT_SUPPORTED ? JDKSAVDECC_AEM_STATUS_NOT_SUPPORTED : status,
                     frame, len, buf);
   pthread_mutex_unlock(&g_probe_lock);
   if (l)
      sendMsg(sock, buf, l);
}

void avdecc_probe_kick(uint64_t eid)
{
   pthread_mutex_lock(&g_probe_lock);
   struct ppeer *p = probe_find(eid);
   if (p)
      p->kicked = 1;
   pthread_mutex_unlock(&g_probe_lock);
}

void avdecc_probe_kick_sid(const uint8_t sid[8])
{
   pthread_mutex_lock(&g_probe_lock);
   for (int i = 0; i < g_n_probe; i++)
      if (!memcmp(g_probe[i].mac, sid, MACLEN))
         g_probe[i].kicked = 1;
   pthread_mutex_unlock(&g_probe_lock);
}

void avdecc_probe_note_connect_tx(uint64_t listener_eid, uint16_t listener_uid, uint16_t talker_uid)
{
   pthread_mutex_lock(&g_probe_lock);
   struct ppeer *p = probe_find(listener_eid);
   if (p) {
      p->kicked = 1;
      if (listener_uid < PROBE_STREAMS) {
         p->in[listener_uid].ctx_ms = get_time_in_ms();
         p->in[listener_uid].ctx_k = talker_uid;
      }
   }
   pthread_mutex_unlock(&g_probe_lock);
}

int avdecc_probe_anomalies(struct avdecc_diag *out, int max)
{
   int n = 0;

   pthread_mutex_lock(&g_probe_lock);
   for (int i = 0; i < g_n_probe; i++) {
      const struct ppeer *p = &g_probe[i];
      if (!p->present)
         continue;
      for (int k = 0; k < p->n_diag && n < max; k++) {
         if (!p->diag[k].shown)
            continue;
         out[n].eid = p->eid;
         out[n].kind = p->diag[k].kind;
         out[n].severe = p->diag[k].severe;
         snprintf(out[n].scope, sizeof out[n].scope, "%s", p->diag[k].scope);
         snprintf(out[n].cause, sizeof out[n].cause, "%s", p->diag[k].cause);
         n++;
      }
   }
   pthread_mutex_unlock(&g_probe_lock);
   return n;
}
