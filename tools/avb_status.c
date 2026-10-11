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
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <jdksavdecc_adp.h>

#include "avb_log.h"
#include "avb_status.h"
#include "utils.h"
#include "msrp_talker.h"
#include "msrp_listener.h"
#include "msrp_domain.h"
#include "descriptor.h"
#include "avdecc_peers.h"
#include "avdecc_probe.h"
#include "gptp_link.h"

static int g_tty = 0;

#define C_RESET  "\033[0m"
#define C_GRN    "\033[32m"
#define C_YEL    "\033[33m"
#define C_RED    "\033[31m"
#define C_DIM    "\033[2m"
#define C_CYAN   "\033[36m"
#define C_MAG    "\033[35m"
#define C_BLU    "\033[34m"

static const char *glyph(int lvl)
{
   return lvl >= 2 ? C_GRN "●" C_RESET
        : lvl == 1 ? C_YEL "◐" C_RESET
                   : C_RED "○" C_RESET;
}

static const char *arrow_tx(int bound)
{
   return bound ? C_CYAN "→" C_RESET : C_DIM "×" C_RESET;
}
static const char *arrow_rx(int bound)
{
   return bound ? C_CYAN "←" C_RESET : C_DIM "×" C_RESET;
}

static void fmt_hhmmss(uint64_t ms, char *out, size_t n)
{
   unsigned s = (unsigned)(ms / 1000);
   snprintf(out, n, "%02u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
}

static void fmt_relative_seconds(uint64_t ms, char *out, size_t n)
{
   snprintf(out, n, "%.1fs", ms / 1000.0);
}

static int mac_is_zero(const uint8_t *m)
{
   return !(m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

struct rsv_summary { int ready, pending, failed, idle, lvl; };

static struct rsv_summary talker_summary(const struct avb_status *s)
{
   struct rsv_summary r = {0};
   for (int k = 0; k < s->n_streams; k++) {
      switch (s->talker[k].state) {
      case TK_READY:       r.ready++;   break;
      case TK_ADVERTISING: r.pending++; break;
      case TK_FAILED:      r.failed++;  break;
      default:             r.idle++;    break;
      }
   }
   r.lvl = r.failed ? 0 : (s->n_streams && r.ready == s->n_streams) ? 2 : 1;
   return r;
}

static struct rsv_summary listener_summary(const struct avb_status *s)
{
   struct rsv_summary r = {0};
   for (int k = 0; k < s->n_streams; k++) {
      switch (s->listener[k].state) {
      case LS_READY:     r.ready++;   break;
      case LS_ASKING:    r.pending++; break;
      case LS_ASKFAILED:
      case LS_FAILED:    r.failed++;  break;
      default:           r.idle++;    break;
      }
   }
   r.lvl = r.failed ? 0 : (r.ready && r.ready + r.idle == s->n_streams) ? 2 : 1;
   return r;
}

static void term_size(int *cols, int *rows)
{
   struct winsize ws;
   if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col) {
      *cols = ws.ws_col;
      *rows = ws.ws_row;
   } else {
      *cols = 0;
      *rows = 0;
   }
}

static int g_compact;
static void blank(void)
{
   if (!g_compact)
      printf("\033[K\n");
}

struct sbuf { char *p; size_t left; };

static void sb_add(struct sbuf *b, const char *fmt, ...)
{
   va_list ap;
   int n;

   va_start(ap, fmt);
   n = vsnprintf(b->p, b->left, fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   if ((size_t)n >= b->left)
      n = b->left ? (int)b->left - 1 : 0;
   b->p += n;
   b->left -= n;
}

static int vis_width(const char *s)
{
   int w = 0;
   while (*s) {
      if (*s == '\033') {
         while (*s && *s != 'm')
            s++;
         if (*s)
            s++;
         continue;
      }
      if (((unsigned char)*s & 0xc0) != 0x80)
         w++;
      s++;
   }
   return w;
}

static void fit_cols(char *s, int w)
{
   int cols = 0;
   char *cut = NULL;

   for (char *p = s; *p; p++) {
      if (((unsigned char)*p & 0xc0) == 0x80)
         continue;
      if (cols == w - 1)
         cut = p;
      if (++cols > w) {
         strcpy(cut, "…");
         return;
      }
   }
}

/* s needs room for 7 more bytes: "…" and the reset */
static void fit_vis(char *s, int w)
{
   int cols = 0;

   if (w <= 0 || vis_width(s) <= w)
      return;
   for (char *p = s; *p; ) {
      if (*p == '\033') {
         while (*p && *p != 'm')
            p++;
         if (*p)
            p++;
         continue;
      }
      if (cols++ == w - 1) {
         strcpy(p, "…" C_RESET);
         return;
      }
      p++;
      while (((unsigned char)*p & 0xc0) == 0x80)
         p++;
   }
}

/* a fmt() result stays valid only for the next FMT_RING calls */
#define FMT_RING 64
#define FMT_LEN  160

static const char *fmt(const char *f, ...)
{
   static char ring[FMT_RING][FMT_LEN];
   static int next;
   char *out = ring[next++ % FMT_RING];
   va_list ap;

   va_start(ap, f);
   vsnprintf(out, FMT_LEN, f, ap);
   va_end(ap);
   return out;
}

#define CHG_HOLD  2
#define CHG_SLOTS 4096
#define CHG_RING  64
#define CHG_LEN   256

static struct { uint64_t key, val; unsigned frame; } g_chg[CHG_SLOTS];
static unsigned g_frame;

static uint64_t fnv1a(const char *s)
{
   uint64_t h = 0xcbf29ce484222325ULL;
   while (*s) {
      h ^= (unsigned char)*s++;
      h *= 0x100000001b3ULL;
   }
   return h;
}

static int chg_lit(uint64_t key, uint64_t val)
{
   if (!key)
      key = 1;
   for (unsigned n = 0, i = key % CHG_SLOTS; n < CHG_SLOTS; n++, i = (i + 1) % CHG_SLOTS) {
      if (g_chg[i].key == key) {
         if (g_chg[i].val != val) {
            g_chg[i].val = val;
            g_chg[i].frame = g_frame;
         }
         return g_frame - g_chg[i].frame < CHG_HOLD;
      }
      if (!g_chg[i].key) {
         g_chg[i].key = key;
         g_chg[i].val = val;
         g_chg[i].frame = g_frame > 1 ? g_frame : g_frame - CHG_HOLD;
         return g_frame > 1;
      }
   }
   return 0;
}

static const char *chg_mark(const char *text)
{
   static char ring[CHG_RING][CHG_LEN];
   static int next;
   const char *p = text, *start = NULL, *end = NULL;

   while (*p) {
      if (*p == '\033') {
         while (*p && *p != 'm')
            p++;
         if (*p)
            p++;
         continue;
      }
      int sp = *p == ' ';
      if (!sp && !start)
         start = p;
      p++;
      while (((unsigned char)*p & 0xc0) == 0x80)
         p++;
      if (!sp)
         end = p;
   }
   if (!start)
      return text;

   char *out = ring[next++ % CHG_RING];
   struct sbuf b = { out, CHG_LEN };
   sb_add(&b, "%.*s\033[7m", (int)(start - text), text);
   for (p = start; p < end; ) {
      const char *r = strstr(p, C_RESET);
      if (!r || r >= end) {
         sb_add(&b, "%.*s", (int)(end - p), p);
         break;
      }
      sb_add(&b, "%.*s\033[0;7m", (int)(r - p), p);
      p = r + strlen(C_RESET);
   }
   sb_add(&b, "\033[27m%s", end);
   return b.left > 1 ? out : text;
}

static const char *chg(const char *text, const char *keyfmt, ...)
{
   char key[64];
   va_list ap;

   va_start(ap, keyfmt);
   vsnprintf(key, sizeof key, keyfmt, ap);
   va_end(ap);
   return chg_lit(fnv1a(key), fnv1a(text)) ? chg_mark(text) : text;
}

static const char *chg_as(const char *cls, const char *text, const char *keyfmt, ...)
{
   char key[64];
   va_list ap;

   va_start(ap, keyfmt);
   vsnprintf(key, sizeof key, keyfmt, ap);
   va_end(ap);
   return chg_lit(fnv1a(key), fnv1a(cls)) ? chg_mark(text) : text;
}

static void rule(int cols)
{
   int n = cols > 2 ? cols - 2 : 79;

   printf("\033[K " C_DIM);
   for (int i = 0; i < n; i++)
      fputs("═", stdout);
   printf(C_RESET "\n");
}

#define STREAM_ROW_W   92
#define STREAM_PEERS_W 4
#define STREAM_INDENT  "   "
#define STREAM_COL_GAP "    "
#define STREAM_SPLIT_N 16

static char peer_label(int idx)
{
   return idx < 26 ? 'A' + idx : idx < 52 ? 'a' + idx - 26 : '?';
}

static int peer_index(const uint64_t *peers, int n, uint64_t id)
{
   for (int i = 0; i < n; i++)
      if (peers[i] == id)
         return i;
   return -1;
}

static void stream_hdr(struct sbuf *b)
{
   sb_add(b, C_DIM "%2s %-4s  %-9s %s %-9s %s %-8s %6s %10s %5s %5s %5s %6s" C_RESET,
          "#", "sid", "OUT", "→    ", "IN", "←    ", "rx path", "pkt/s", "pkts", "drops", "late", "align", "drift");
}

static void rsv_cell(struct sbuf *b, const char *col, const char *txt, int ready, int attempts,
                     const char *key, int i)
{
   char t[16];
   if (!ready && attempts > 0)
      snprintf(t, sizeof t, "%s %d/%d", txt, attempts, MSRP_MAX_ATTEMPTS);
   else
      snprintf(t, sizeof t, "%s", txt);
   sb_add(b, "%s", chg(fmt("%s%-9s" C_RESET, col, t), "s%d.%s", i, key));
}

static void stream_row(struct sbuf *b, const struct avb_status *s, int i,
                       const uint64_t *peers, int n_peers)
{
   const struct avb_status_stream *st = &s->streams[i];
   enum talker_state ts = s->talker[i].state;
   enum listener_rsv ls = s->listener[i].state;

   sb_add(b, "%02d %s  ", i, chg(fmt("%04x", (unsigned)(st->stream_id & 0xffff)), "s%d.sid", i));

   rsv_cell(b, ts == TK_READY ? C_GRN : ts == TK_ADVERTISING ? C_YEL : ts == TK_FAILED ? C_RED : C_DIM,
               ts == TK_READY ? "READY"  : ts == TK_ADVERTISING ? "ADV"  : ts == TK_FAILED ? "FAIL"  : "IDLE",
            ts == TK_READY, s->talker[i].attempts, "tk", i);
   char tl[STREAM_PEERS_W + 1] = "";
   if (s->conn[i].tx_bound) {
      int n = s->conn[i].tx_n, j;
      for (j = 0; j < n && j < STATUS_TX_PEERS && j < STREAM_PEERS_W - 1; j++)
         tl[j] = peer_label(peer_index(peers, n_peers, s->conn[i].tx_peers[j]));
      if (!n)
         tl[j++] = peer_label(peer_index(peers, n_peers, s->conn[i].tx_peer));
      else if (n > STREAM_PEERS_W - 1)
         tl[j++] = '+';
      tl[j] = '\0';
   }
   sb_add(b, " %s%*s ", chg(fmt("%s%s", arrow_tx(s->conn[i].tx_bound), tl), "s%d.txp", i),
          STREAM_PEERS_W - (int)strlen(tl), "");

   rsv_cell(b, ls == LS_READY ? C_GRN : ls == LS_ASKING ? C_YEL
             : ls == LS_ASKFAILED || ls == LS_FAILED ? C_RED : C_DIM,
               ls == LS_READY ? "READY" : ls == LS_ASKING ? "ASK" : ls == LS_ASKFAILED ? "AFAIL"
             : ls == LS_FAILED ? "FAIL" : "IDLE",
            ls == LS_READY, s->listener[i].attempts, "ls", i);
   char rl[STREAM_PEERS_W + 1] = "";
   if (s->conn[i].rx_bound) {
      int sh = -1;
      for (int j = 0; s->conn[i].rx_sid && j < i && sh < 0; j++)
         if (s->conn[j].rx_bound && s->conn[j].rx_sid == s->conn[i].rx_sid)
            sh = j;
      char pl = peer_label(peer_index(peers, n_peers, s->conn[i].rx_peer));
      if (sh >= 0)
         snprintf(rl, sizeof rl, "%c=%02d", pl, sh);
      else
         snprintf(rl, sizeof rl, "%c", pl);
   }
   sb_add(b, " %s%*s ", chg(fmt("%s%s", arrow_rx(s->conn[i].rx_bound), rl), "s%d.rxp", i),
          STREAM_PEERS_W - (int)strlen(rl), "");

   if (mac_is_zero(st->rx_src_mac) || mac_is_zero(st->rx_dst_mac)) {
      sb_add(b, "%s", chg(fmt(C_DIM "%-8s" C_RESET, "unbound"), "s%d.path", i));
      sb_add(b, C_DIM " %6s %10llu %5llu %5llu %+4df %6s" C_RESET,
             "-", (unsigned long long)st->rx_pkts,
             (unsigned long long)st->rx_drops, (unsigned long long)st->rx_late,
             st->align_offset_frames, "-");
      return;
   }

   char where[16];
   if (st->rx_filter_id >= 0)
      snprintf(where, sizeof where, "filter#%d", st->rx_filter_id);
   else
      snprintf(where, sizeof where, "sw-demux");
   sb_add(b, "%s ", chg(fmt("%-8s", where), "s%d.path", i));

   int silent = !st->rx_pkts_per_s;
   const char *pps_cls = !silent ? "live" : s->rx_capture_running ? "silent" : "idle";
   if (!silent)
      sb_add(b, "%s ", chg_as(pps_cls, fmt("%6u", st->rx_pkts_per_s), "s%d.pps", i));
   else if (s->rx_capture_running)
      sb_add(b, "%s ", chg_as(pps_cls, C_YEL "SILENT" C_RESET, "s%d.pps", i));
   else
      sb_add(b, "%s ", chg_as(pps_cls, fmt(C_DIM "%6s" C_RESET, "idle"), "s%d.pps", i));

   sb_add(b, "%10llu %s %s %s ",
          (unsigned long long)st->rx_pkts,
          chg(fmt("%s%5llu" C_RESET, st->rx_drops ? C_RED : "", (unsigned long long)st->rx_drops),
              "s%d.drops", i),
          chg(fmt("%s%5llu" C_RESET, st->rx_late ? C_YEL : "", (unsigned long long)st->rx_late),
              "s%d.late", i),
          chg(fmt("%+4df", st->align_offset_frames), "s%d.align", i));

   int drift_bad = s->rx_drift_warn_ppb && llabs(st->rx_drift_ppb_sliding) > s->rx_drift_warn_ppb;
   const char *drift_col = silent || !st->rx_window_ready ? C_DIM : drift_bad ? C_YEL : "";
   sb_add(b, "%s", chg_as(drift_col, fmt("%s%+6lld" C_RESET, drift_col, st->rx_drift_ppb_sliding),
                          "s%d.drift", i));
}

enum { DC_LABEL, DC_STATE, DC_NAME, DC_CONN, DC_EID, DC_MAC, DC_ROLE, DC_SRCSNK, DC_GM,
       DC_SERIAL, DC_GROUP, DC_CFG, DC_AVAIL, DC_ADP, DC_N };

static const struct { const char *hdr; int cap, right; } dev_col[DC_N] = {
   [DC_LABEL]  = { "#",         1, 0 }, [DC_STATE]  = { "",          1, 0 },
   [DC_NAME]   = { "name",     20, 0 }, [DC_CONN]   = { "→out ←in",  0, 0 },
   [DC_EID]    = { "entity id", 0, 0 }, [DC_MAC]    = { "mac",       0, 0 },
   [DC_ROLE]   = { "role",      0, 0 }, [DC_SRCSNK] = { "src/snk",   0, 0 },
   [DC_GM]     = { "GM",        0, 0 }, [DC_SERIAL] = { "serial",   16, 0 },
   [DC_GROUP]  = { "group",    16, 0 }, [DC_CFG]    = { "cfg",       0, 0 },
   [DC_AVAIL]  = { "avail",     0, 1 }, [DC_ADP]    = { "ADP",       0, 1 },
};

#define DEV_ROWS (2 * MAX_STREAMS + AVDECC_PEERS_MAX)
#define DEV_CELL 160
#define DEV_DIAG 512

static void dev_cell(char *cell, int c, const char *col, const char *txt)
{
   char t[DEV_CELL - 16];
   snprintf(t, sizeof t, "%s", txt);
   if (dev_col[c].cap)
      fit_cols(t, dev_col[c].cap);
   snprintf(cell, DEV_CELL, "%s%s%s", col ? col : "", t, col ? C_RESET : "");
}

static void dev_fill(char cell[DC_N][DEV_CELL], const struct avb_status *s,
                     const struct avdecc_peer_info *d, uint64_t eid,
                     char label, int n_out, int n_in, int diag, uint64_t now)
{
   char t[96];
   int full = !diag;

   memset(cell, 0, DC_N * DEV_CELL);
   dev_cell(cell[DC_LABEL], DC_LABEL, C_CYAN, (char[]){ label, 0 });

   if (n_out || n_in) {
      char o[12] = "    ", i[12] = "   ";
      if (n_out) snprintf(o, sizeof o, "→%02d ", n_out);
      if (n_in)  snprintf(i, sizeof i, "←%02d", n_in);
      snprintf(t, sizeof t, "%s %s", o, i);
      dev_cell(cell[DC_CONN], DC_CONN, NULL, t);
   }
   snprintf(t, sizeof t, "%016llx", (unsigned long long)eid);
   dev_cell(cell[DC_EID], DC_EID, d ? NULL : C_DIM, t);

   if (!d) {
      snprintf(cell[DC_STATE], DEV_CELL, C_RED "○" C_RESET);
      dev_cell(cell[DC_NAME], DC_NAME, C_DIM, "-");
      dev_cell(cell[DC_ADP], DC_ADP, C_RED, "never");
      return;
   }

   uint64_t age = now > d->last_adp_ms ? now - d->last_adp_ms : 0;
   int silent = age > d->valid_ms;
   snprintf(cell[DC_STATE], DEV_CELL, "%s", d->departed ? C_RED "○" C_RESET
                                          : silent ? C_YEL "◐" C_RESET
                                          : diag ? C_YEL "●" C_RESET : C_GRN "●" C_RESET);
   if (d->departed)
      dev_cell(cell[DC_ADP], DC_ADP, C_RED, "gone");
   else {
      snprintf(t, sizeof t, "%.1fs", age / 1000.0);
      dev_cell(cell[DC_ADP], DC_ADP, silent ? C_YEL : NULL, t);
   }

   if (d->have_aem && d->name[0])
      dev_cell(cell[DC_NAME], DC_NAME, NULL, d->name);
   else
      dev_cell(cell[DC_NAME], DC_NAME, C_DIM,
               d->have_aem ? "(unnamed)"
               : n_out || n_in || (d->entity_caps & JDKSAVDECC_ADP_ENTITY_CAPABILITY_AEM_SUPPORTED)
               ? "(asking)" : "(no AEM)");

   snprintf(t, sizeof t, "%02x:%02x:%02x:%02x:%02x:%02x",
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
   dev_cell(cell[DC_MAC], DC_MAC, NULL, t);

   snprintf(t, sizeof t, "%c%c%c",
            d->talker_caps & JDKSAVDECC_ADP_TALKER_CAPABILITY_IMPLEMENTED ? 'T' : '-',
            d->listener_caps & JDKSAVDECC_ADP_LISTENER_CAPABILITY_IMPLEMENTED ? 'L' : '-',
            d->controller_caps & JDKSAVDECC_ADP_CONTROLLER_CAPABILITY_IMPLEMENTED ? 'C' : '-');
   dev_cell(cell[DC_ROLE], DC_ROLE, NULL, t);
   if (d->talker_sources || d->listener_sinks) {
      snprintf(t, sizeof t, " %02u/%02u", d->talker_sources, d->listener_sinks);
      dev_cell(cell[DC_SRCSNK], DC_SRCSNK, NULL, t);
   }

   if (!(d->entity_caps & JDKSAVDECC_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED) || !d->gptp_gm)
      dev_cell(cell[DC_GM], DC_GM, C_DIM, "-");
   else if (d->gptp_gm == s->gptp_gm && d->gptp_domain == s->gptp_domain)
      dev_cell(cell[DC_GM], DC_GM, C_GRN, "ours");
   else {
      snprintf(t, sizeof t, "%016llx dom%u", (unsigned long long)d->gptp_gm, d->gptp_domain);
      dev_cell(cell[DC_GM], DC_GM, C_YEL, t);
   }

   if (!full)
      return;
   if (d->have_aem) {
      dev_cell(cell[DC_SERIAL], DC_SERIAL, NULL, d->serial);
      dev_cell(cell[DC_GROUP], DC_GROUP, NULL, d->group);
      if (d->config_count > 1) {
         snprintf(t, sizeof t, "%u of %u", d->config_cur, d->config_count);
         dev_cell(cell[DC_CFG], DC_CFG, NULL, t);
      }
   }
   snprintf(t, sizeof t, "%u", d->available_index);
   dev_cell(cell[DC_AVAIL], DC_AVAIL, NULL, t);
}

static int diag_text(const struct avdecc_diag *dg, int n_diag, uint64_t eid, char *buf, size_t n)
{
   struct sbuf b = { buf, n };
   int count = 0, severe = 0;

   buf[0] = '\0';
   for (int sev = 1; sev >= 0; sev--)
      for (int i = 0; i < n_diag; i++) {
         if (dg[i].eid != eid || !!dg[i].severe != sev)
            continue;
         sb_add(&b, "%s%s%s%s", count++ ? "; " : "", dg[i].scope, dg[i].scope[0] ? ": " : "", dg[i].cause);
         severe |= dg[i].severe;
      }
   return count ? 1 + severe : 0;
}

static void dev_row(char cell[DC_N][DEV_CELL], char *diag_out, const struct avb_status *s,
                    const struct avdecc_peer_info *d, uint64_t eid, char label, int n_out, int n_in,
                    const struct avdecc_diag *dg, int n_diag, uint64_t now)
{
   char txt[DEV_DIAG];
   int diag = d && !d->departed ? diag_text(dg, n_diag, eid, txt, sizeof txt) : 0;

   dev_fill(cell, s, d, eid, label, n_out, n_in, diag, now);
   diag_out[0] = '\0';
   if (diag) {
      cell[DC_ADP][0] = '\0';
      snprintf(diag_out, DEV_DIAG, "%s", chg(fmt("%s%.140s" C_RESET, diag == 2 ? C_RED : C_YEL, txt),
                                              "d%016llx.diag", (unsigned long long)eid));
   }
   for (int c = 0; c < DC_N; c++) {
      if (c == DC_ADP || c == DC_AVAIL)
         continue;
      const char *t = chg(cell[c], "d%016llx.%d", (unsigned long long)eid, c);
      if (t != cell[c])
         snprintf(cell[c], DEV_CELL, "%s", t);
   }
}

static int device_lines(int n_rows)
{
   return n_rows ? 2 + n_rows : 1;
}


static void render_devices(const struct avb_status *s, const uint64_t *peers,
                           const int *n_out, const int *n_in, int n_peers,
                           const struct avdecc_peer_info *devs, int n_devs,
                           const struct avdecc_diag *dg, int n_diag, int cols)
{
   static char cell[DEV_ROWS][DC_N][DEV_CELL];
   static char diag[DEV_ROWS][DEV_DIAG];
   int width[DC_N] = {0};
   int n_rows = 0, n_online = 0, n_bad = 0;
   uint64_t now = get_time_in_ms();

   for (int p = 0; p < n_peers && n_rows < DEV_ROWS; p++) {
      const struct avdecc_peer_info *d = NULL;
      for (int k = 0; k < n_devs && !d; k++)
         if (devs[k].eid == peers[p])
            d = &devs[k];
      dev_row(cell[n_rows], diag[n_rows], s, d, peers[p], peer_label(p), n_out[p], n_in[p], dg, n_diag, now);
      n_bad += !!diag[n_rows++][0];
   }
   for (int k = 0; k < n_devs && n_rows < DEV_ROWS; k++)
      if (peer_index(peers, n_peers, devs[k].eid) < 0) {
         dev_row(cell[n_rows], diag[n_rows], s, &devs[k], devs[k].eid, ' ', 0, 0, dg, n_diag, now);
         n_bad += !!diag[n_rows++][0];
      }
   for (int k = 0; k < n_devs; k++)
      if (!devs[k].departed && now - devs[k].last_adp_ms <= devs[k].valid_ms)
         n_online++;

   printf("\033[K   DEVICES (ADP)   %s discovered   %s online   %s connected   %s%s\n",
          chg(fmt("%02d", n_devs), "dev.n"), chg(fmt("%02d", n_online), "dev.online"),
          chg(fmt("%02d", n_peers), "dev.conn"),
          chg(n_bad ? fmt(C_YEL "%02d with problems" C_RESET " (journal: [DIAG])", n_bad) : C_DIM "no problems" C_RESET,
              "dev.bad"),
          n_rows ? "" : C_DIM "   (none)" C_RESET);
   if (!n_rows)
      return;

   for (int r = 0; r < n_rows; r++)
      for (int c = 0; c < DC_N; c++)
         if (vis_width(cell[r][c]) > width[c])
            width[c] = vis_width(cell[r][c]);
   for (int c = 0; c < DC_N; c++)
      if (width[c] && vis_width(dev_col[c].hdr) > width[c])
         width[c] = vis_width(dev_col[c].hdr);

   int last = DC_N - 1;
   while (last > 0 && !width[last])
      last--;

   char line[DC_N * (DEV_CELL + 32)];
   for (int r = -1; r < n_rows; r++) {
      struct sbuf b = { line, sizeof line };
      sb_add(&b, STREAM_INDENT);
      if (r < 0)
         sb_add(&b, C_DIM);
      for (int c = 0; c < DC_N; c++) {
         if (r >= 0 && diag[r][0] && c == DC_SERIAL) {
            sb_add(&b, "%s", diag[r]);
            break;
         }
         if (!width[c])
            continue;
         const char *txt = r < 0 ? dev_col[c].hdr : cell[r][c];
         int pad = width[c] - vis_width(txt);
         if (c == last && !dev_col[c].right && !(r >= 0 && diag[r][0])) {
            sb_add(&b, "%s", txt);
            break;
         }
         if (dev_col[c].right)
            sb_add(&b, "%*s%s ", pad, "", txt);
         else
            sb_add(&b, "%s%*s ", txt, pad, "");
         if (c == DC_LABEL || c == DC_STATE)
            continue;
         sb_add(&b, " ");
      }
      if (r < 0)
         sb_add(&b, C_RESET);
      for (char *e = b.p; e > line && e[-1] == ' '; )
         *--e = '\0';
      if (cols)
         fit_vis(line, cols - 1);
      printf("\033[K%s\n", line);
   }
}

static int stream_cols(int n, int cols)
{
   return n > STREAM_SPLIT_N &&
          cols >= (int)strlen(STREAM_INDENT) + 2 * STREAM_ROW_W + (int)strlen(STREAM_COL_GAP)
          ? 2 : 1;
}

static void render_streams(const struct avb_status *s, int rx_live, int cols,
                           const struct avdecc_peer_info *devs, int n_devs,
                           const struct avdecc_diag *dg, int n_diag)
{
   uint64_t peers[2 * MAX_STREAMS];
   int n_out[2 * MAX_STREAMS] = {0}, n_in[2 * MAX_STREAMS] = {0};
   int n_peers = 0, n_tx_bound = 0, n_rx_bound = 0;
   unsigned long long drops = 0, late = 0;

   for (int i = 0; i < s->n_streams; i++) {
      uint64_t ids[STATUS_TX_PEERS + 1];
      int n_tx = s->conn[i].tx_n < STATUS_TX_PEERS ? s->conn[i].tx_n : STATUS_TX_PEERS;
      memcpy(ids, s->conn[i].tx_peers, n_tx * sizeof ids[0]);
      if (!n_tx)
         ids[n_tx++] = s->conn[i].tx_peer;
      ids[n_tx] = s->conn[i].rx_peer;
      for (int e = 0; e <= n_tx; e++) {
         int dir = e == n_tx;
         if (!(dir ? s->conn[i].rx_bound : s->conn[i].tx_bound))
            continue;
         int p = peer_index(peers, n_peers, ids[e]);
         if (p < 0) {
            if (n_peers == (int)(sizeof peers / sizeof peers[0]))
               continue;
            p = n_peers++;
            peers[p] = ids[e];
         }
         if (dir) n_in[p]++;
         else     n_out[p]++;
      }
      n_tx_bound += !!s->conn[i].tx_bound;
      n_rx_bound += !!s->conn[i].rx_bound;
      drops += s->streams[i].rx_drops;
      late  += s->streams[i].rx_late;
   }

   printf("\033[K   STREAMS (ACMP)  %s   OUT %s bound   IN %s bound   RX live %s%s"
          "   unmatched %s   drops %s   late %s\n",
          chg(fmt("%d stream%s", s->n_streams, s->n_streams == 1 ? "" : "s"), "st.n"),
          chg(fmt("%02d/%02d", n_tx_bound, s->n_streams), "st.out"),
          chg(fmt("%02d/%02d", n_rx_bound, s->n_streams), "st.in"),
          chg(fmt("%02d/%02d", rx_live, s->n_streams), "st.live"),
          chg(s->rx_capture_running ? "" : C_DIM " (capture idle)" C_RESET, "st.cap"),
          chg(fmt("%s%llu" C_RESET, s->rx_unmatched ? C_YEL : "", (unsigned long long)s->rx_unmatched), "st.unm"),
          chg(fmt("%s%llu" C_RESET, drops ? C_RED : "", drops), "st.drops"),
          chg(fmt("%s%llu" C_RESET, late ? C_YEL : "", late), "st.late"));

   const uint8_t *bm = s->streams[0].dest_mac;
   printf("\033[K                   base sid 0x%s   dest %s\n",
          chg(fmt("%016llx", (unsigned long long)s->streams[0].stream_id), "st.sid"),
          chg(fmt("%02x:%02x:%02x:%02x:%02x:%02x", bm[0], bm[1], bm[2], bm[3], bm[4], bm[5]), "st.dest"));

   render_devices(s, peers, n_out, n_in, n_peers, devs, n_devs, dg, n_diag, cols);

   int ncol = stream_cols(s->n_streams, cols);
   int h = (s->n_streams + ncol - 1) / ncol;
   char line[1024];
   for (int r = -1; r < h; r++) {
      struct sbuf b = { line, sizeof line };
      sb_add(&b, STREAM_INDENT);
      for (int c = 0; c < ncol; c++) {
         int i = c * h + r;
         if (r >= 0 && i >= s->n_streams)
            break;
         if (c) {
            int pad = STREAM_ROW_W - (vis_width(line) - (int)strlen(STREAM_INDENT)
                      - (c - 1) * (STREAM_ROW_W + (int)strlen(STREAM_COL_GAP)));
            sb_add(&b, "%*s" STREAM_COL_GAP, pad > 0 ? pad : 0, "");
         }
         if (r < 0)
            stream_hdr(&b);
         else
            stream_row(&b, s, i, peers, n_peers);
      }
      printf("\033[K%s\n", line);
   }
}

void status_init(void)
{
   g_tty = isatty(STDOUT_FILENO);
   if (g_tty) { printf("\033[2J\033[H\033[?25l"); fflush(stdout); }
}

void status_screen_teardown(void)
{
   if (g_tty) { printf("\033[?25h\n"); fflush(stdout); }
}

struct avb_status g_status;
pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;

void status_render(const char *iface, uint64_t start_ms)
{
   struct avb_status s;
   pthread_mutex_lock(&g_status_lock);
   s = g_status;
   pthread_mutex_unlock(&g_status_lock);

   struct rsv_summary tk = talker_summary(&s);
   struct rsv_summary ls = listener_summary(&s);
   int rx_live = 0;
   for (int i = 0; i < s.n_streams; i++)
      if (s.streams[i].rx_pkts_per_s)
         rx_live++;

   if (!g_tty) {
      int pcm_channels = s.n_streams * CHANNELS_PER_STREAM;

      avb_log(L_SYS, LOG_INFO,
              "clock=%s talker=ready %d/%d (failed %d) listener=ready %d/%d (failed %d) rx-live %d/%d avtp=%s %dch (%d streams) silence=%llu ctrls=%d",
              s.gptp_synced ? "synced" : s.gptp_locked ? "syncing" : "down",
              tk.ready, s.n_streams, tk.failed,
              ls.ready, s.n_streams, ls.failed,
              rx_live, s.n_streams,
              s.tx_gated ? "gated" : "armed",
              pcm_channels, s.n_streams,
              (unsigned long long)s.tx_silence_packets,
              s.controllers_seen);
      return;
   }

   g_frame++;
   char up[16], last[256 + 8], adp[32];   /* + room for fit_vis()'s "…" */
   fmt_hhmmss(get_time_in_ms() - start_ms, up, sizeof up);
   avb_log_last(last, sizeof last);
   if (s.last_adp_ms) snprintf(adp, sizeof adp, "%.1fs ago", (get_time_in_ms() - s.last_adp_ms) / 1000.0);
   else               snprintf(adp, sizeof adp, "never");

   int ck_lvl = s.gptp_synced ? 2 : s.gptp_locked ? 1 : 0;

   static struct avdecc_peer_info devs[AVDECC_PEERS_MAX];
   int n_devs = avdecc_peers_list(devs, AVDECC_PEERS_MAX);
   static struct avdecc_diag diags[AVDECC_DIAG_MAX];
   int n_diag = avdecc_probe_anomalies(diags, AVDECC_DIAG_MAX);

   int cols, rows;
   term_size(&cols, &rows);
   int ncol = stream_cols(s.n_streams, cols);
   g_compact = rows && 34 + device_lines(n_devs) + (s.n_streams + ncol - 1) / ncol > rows;

    printf("\033[H");
    const char *entity_name = descriptor_get_entity_name();
    printf("\033[K " C_DIM "AVB4linux" C_RESET "  %s  %s  %s"
          "               " C_BLU "Uptime:" C_RESET " %s\n", chg(entity_name, "hdr.name"),
           iface,
           chg(fmt("%02x:%02x:%02x:%02x:%02x:%02x",
                   (unsigned)((g_status.maap_base >> 40) & 0xff), (unsigned)((g_status.maap_base >> 32) & 0xff),
                   (unsigned)((g_status.maap_base >> 24) & 0xff), (unsigned)((g_status.maap_base >> 16) & 0xff),
                   (unsigned)((g_status.maap_base >>  8) & 0xff), (unsigned)( g_status.maap_base        & 0xff)),
               "hdr.mac"), up);
   rule(cols);
   blank();
   
   char clkoff[40];
   if (s.gptp_synced) {
       snprintf(clkoff, sizeof clkoff, "off % 6lldns (PHC disciplined)", (long long)s.gptp_offset_ns);
   } else if (s.gptp_locked) {
       snprintf(clkoff, sizeof clkoff, "off % 6lldns (PHC converging)", (long long)s.gptp_offset_ns);
   } else {
       clkoff[0] = '\0';
   }
   char clkstats[96] = "";
   if (s.gptp_offset_samples > 0 && s.gptp_synced) {
       int64_t avg = s.gptp_offset_sum_ns / (int64_t)s.gptp_offset_samples;
       snprintf(clkstats, sizeof clkstats, "  min %sns max %sns avg %lldns",
                chg(fmt("%lld", (long long)s.gptp_offset_min_ns), "clk.min"),
                chg(fmt("%lld", (long long)s.gptp_offset_max_ns), "clk.max"),
                (long long)avg);
   }
   printf("\033[K %s CLOCK  (gPTP)   %s  GM %s  %s\n",
          chg(glyph(ck_lvl), "clk.g"),
          chg(s.gptp_synced ? "LOCKED" : s.gptp_locked ? "SLAVE " : s.gptp_up ? "NO-GM " : "DOWN  ", "clk.st"),
          chg(fmt("%016llx", (unsigned long long)s.gptp_gm), "clk.gm"),
          chg(fmt("dom%u", s.gptp_domain), "clk.dom"));
   char link[96], linkcol[128] = "";
   int link_lvl = gptp_link_text(link, sizeof link);
   if (link[0])
       snprintf(linkcol, sizeof linkcol, "  %s%s" C_RESET,
                link_lvl == 2 ? C_RED : link_lvl == 1 ? C_YEL : C_DIM, link);
   printf("\033[K                     %s%s%s\n", clkoff, clkstats, chg(linkcol, "clk.link"));
   blank();
   
   printf("\033[K %s TALKER (MSRP)   READY %s   advertising %s  failed %s  idle %s\n",
          chg(glyph(tk.lvl), "tk.g"), chg(fmt("%02d/%02d", tk.ready, s.n_streams), "tk.ready"),
          chg(fmt("%02d", tk.pending), "tk.pend"), chg(fmt("%02d", tk.failed), "tk.fail"),
          chg(fmt("%02d", tk.idle), "tk.idle"));
   printf("\033[K %s LISTEN (MSRP)   READY %s   asking %s       failed %s  idle %s\n",
          chg(glyph(ls.lvl), "ls.g"), chg(fmt("%02d/%02d", ls.ready, s.n_streams), "ls.ready"),
          chg(fmt("%02d", ls.pending), "ls.pend"), chg(fmt("%02d", ls.failed), "ls.fail"),
          chg(fmt("%02d", ls.idle), "ls.idle"));
   blank();
   
   char cls[8];
   if (s.domain_class == MSRP_CLASS_A_ID) snprintf(cls, sizeof cls, "A");
   else                                   snprintf(cls, sizeof cls, "%d", s.domain_class);
   printf("\033[K   DOMAIN          %s  class %s  prio %s  vid %s\n",
          chg(s.domain_ok ? "registered" : "pending   ", "dom.st"), chg(cls, "dom.cls"),
          chg(fmt("%d", s.domain_prio), "dom.prio"), chg(fmt("%d", s.domain_vid), "dom.vid"));
   blank();
   
   int pcm_channels = s.n_streams * CHANNELS_PER_STREAM;
   if (s.tx_timer_active)
       printf("\033[K %s AVTP   (1722)   %s   %u ticks/s  %s  pkts/tick=%s\n",
              chg(glyph(s.tx_gated ? 1 : !s.tx_ticks_per_s ? (gptp_link_carrier() ? 0 : 1) : 2), "tx.g"),
              chg(s.tx_gated ? "gated (PHC unsynced)" :
                  !s.tx_ticks_per_s ? (gptp_link_carrier() ? C_RED "STALLED (no launch)" C_RESET " "
                                                       : C_YEL "idle (link down)" C_RESET "    ") : "armed (PHC synced)  ", "tx.st"),
              s.tx_ticks_per_s, chg(fmt("%dch (%d streams)", pcm_channels, s.n_streams), "tx.ch"),
              chg(fmt("%d", s.n_streams), "tx.ppt"));
   else
       printf("\033[K %s AVTP   (1722)   %s  %s\n",
              chg(glyph(2), "tx.g"), chg("RELAY   idle", "tx.st"),
              chg(fmt("%dch (%d streams)", pcm_channels, s.n_streams), "tx.ch"));
   printf("\033[K                   late-launch %s  XRUN %s  gated-ticks %s  silence %llu  %s\n",
          chg(fmt("%llu", (unsigned long long)s.tx_late_launch), "tx.late"),
          chg(fmt("%llu", (unsigned long long)s.tx_xrun), "tx.xrun"),
          chg(fmt("%llu", (unsigned long long)s.tx_gated_ticks), "tx.gated"),
          (unsigned long long)s.tx_silence_packets,
          chg(fmt("%uHz", samplerate), "tx.rate"));
    {
       const char *src = s.media_clk_internal ? "PHC/gPTP (we are master)"
                                              : "input stream";
       int bad = s.rx_engine_seen && s.rx_sliding_ready && s.rx_drift_warn_ppb &&
                 llabs(s.rx_drift_ppb_sliding) > s.rx_drift_warn_ppb;
       const char *settling = !s.rx_engine_seen     ? ""
                            : !s.rx_capture_running ? " (capture idle)"
                            : !s.rx_sliding_ready   ? " (settling)" : "";

       if (!s.rx_engine_seen)
          printf("\033[K %s MEDIA-CLK       %s   %s\n",
                 chg(glyph(2), "mc.g"), chg(src, "mc.src"),
                 chg("peer drift: n/a (no listener)", "mc.drift"));
       else
          printf("\033[K %s MEDIA-CLK       %s   %s%s%s  slips %s  seq-err %s\n",
                 chg(glyph(bad ? 1 : 2), "mc.g"), chg(src, "mc.src"),
                 chg_as(bad ? "bad" : "ok", fmt("peer drift %+lld ppb (s%d)", s.rx_drift_ppb_sliding,
                                                s.rx_drift_stream), "mc.drift"),
                 chg(settling, "mc.settle"), bad ? " NOT FOLLOWING US" : "",
                 chg(fmt("%llu", (unsigned long long)s.rx_clock_slips), "mc.slips"),
                 chg(fmt("%llu", (unsigned long long)s.rx_seq_errors), "mc.seq"));
       if (bad)
          printf("\033[K                   -> set the peer's CLOCK SOURCE to this AVB stream;\n"
                 "\033[K                      its gPTP/'AVB synced' indicator is a different setting\n");
    }
   blank();

    if (s.bw_reserved_bps > 0) {
        const char *icon = s.bw_match == 1 ? C_GRN "●" C_RESET :
                           s.bw_match == 0 ? C_RED "●" C_RESET : C_YEL "●" C_RESET;
        const char *result = s.bw_match == 1 ? "match" :
                             s.bw_match == 0 ? "MISMATCH" : "kernel n/a";
        printf("\033[K %s Reserved BW     MSRP %s Mbps vs CBS idleSlope %s Mbps (%s)\n",
               chg(icon, "bw.g"), chg(fmt("%.3f", s.bw_reserved_bps / 1e6), "bw.msrp"),
               chg(fmt("%.3f", s.bw_cbs_programmed_bps / 1e6), "bw.cbs"), chg(result, "bw.res"));
        if (s.bw_match == 0)
            printf("\033[K                   -> bandwidth advertised to the bridge != kernel shaper reservation\n");
        blank();
    }
   
   char maap_status[32];
   snprintf(maap_status, sizeof maap_status, "%012llx", (unsigned long long)s.maap_base);
   const char *maap_word = s.maap_lvl == MAAP_LVL_DYNAMIC  ? "dynamic"
                         : s.maap_lvl == MAAP_LVL_FALLBACK ? "fallback"
                                                           : "fixed";
   printf("\033[K %s MAAP            %s  %s\n",
          chg(glyph(s.maap_lvl == MAAP_LVL_FALLBACK ? 1 : 2), "maap.g"),
          chg(fmt("%-30s", maap_status), "maap.base"), chg(maap_word, "maap.lvl"));
   blank();
   
   printf("\033[K   AVDECC          controllers %s   last ADP %s   avail_idx %u\n",
          chg(fmt("%02d", s.controllers_seen), "adp.ctl"), adp, s.available_index);
   blank();
   
   printf("\033[K   FQTSS  (Qav)    shaper_daemon (external)   class A reserved\n");
   blank();
   
   rule(cols);
   if (s.n_streams > 0)
      render_streams(&s, rx_live, cols, devs, n_devs, diags, n_diag);
   rule(cols);
   blank();
   if (cols)
      fit_vis(last, cols - 8);
   printf("\033[K " C_DIM "last:" C_RESET " %s\n", chg(last, "last"));
   printf("\033[J");
   fflush(stdout);
}

void status_update_gptp(int up, int locked, int synced, uint64_t gm, int64_t offset, uint8_t domain)
{
   pthread_mutex_lock(&g_status_lock);
   g_status.gptp_up = up;
   g_status.gptp_locked = locked;
   g_status.gptp_synced = synced;
   g_status.gptp_gm = gm;
   g_status.gptp_offset_ns = offset;
   g_status.gptp_domain = domain;
   pthread_mutex_unlock(&g_status_lock);
}

void status_update_talker(int k, enum talker_state state, int attempts, int listeners)
{
   if (k < 0 || k >= MAX_STREAMS)
      return;
   pthread_mutex_lock(&g_status_lock);
   g_status.talker[k].state = state;
   g_status.talker[k].attempts = attempts;
   g_status.talker[k].listeners = listeners;
   pthread_mutex_unlock(&g_status_lock);
}

void status_update_listener(int k, enum listener_rsv state, int attempts)
{
   if (k < 0 || k >= MAX_STREAMS)
      return;
   pthread_mutex_lock(&g_status_lock);
   g_status.listener[k].state = state;
   g_status.listener[k].attempts = attempts;
   pthread_mutex_unlock(&g_status_lock);
}

void status_update_domain(int ok, int class, int prio, int vid)
{
   pthread_mutex_lock(&g_status_lock);
   g_status.domain_ok = ok;
   g_status.domain_class = class;
   g_status.domain_prio = prio;
   g_status.domain_vid = vid;
   pthread_mutex_unlock(&g_status_lock);
}

void status_update_controller(uint64_t controller_id, uint64_t adp_ms)
{
    pthread_mutex_lock(&g_status_lock);
    g_status.last_controller = controller_id;
    g_status.last_adp_ms = adp_ms;
    g_status.controllers_seen++;
    pthread_mutex_unlock(&g_status_lock);
}

void status_update_maap(uint64_t base, int level, int count)
{
     pthread_mutex_lock(&g_status_lock);
     g_status.maap_base = base;
     g_status.maap_lvl = level;
     g_status.maap_count = count;
     pthread_mutex_unlock(&g_status_lock);
}

void status_update_tx_engine(int timer_active, uint32_t ticks_per_s,
                             uint64_t late_launch, uint64_t xrun,
                             int gated, uint64_t gated_ticks,
                             uint64_t silence_packets)
{
     pthread_mutex_lock(&g_status_lock);
     g_status.tx_timer_active = timer_active;
     g_status.tx_ticks_per_s = ticks_per_s;
     g_status.tx_late_launch = late_launch;
     g_status.tx_xrun = xrun;
     g_status.tx_gated = gated;
     g_status.tx_gated_ticks = gated_ticks;
     g_status.tx_silence_packets = silence_packets;
     pthread_mutex_unlock(&g_status_lock);
}

void status_update_rx_engine(int seen, long long drift_ppb, int warn_ppb,
                             uint64_t clock_slips, uint64_t seq_errors,
                             uint64_t unmatched, int capture_running)
{
     pthread_mutex_lock(&g_status_lock);
     g_status.rx_engine_seen    = seen;
     g_status.rx_capture_running = capture_running;
     g_status.rx_drift_ppb      = drift_ppb;
     g_status.rx_drift_warn_ppb = warn_ppb;
     g_status.rx_clock_slips    = clock_slips;
     g_status.rx_seq_errors     = seq_errors;
     g_status.rx_unmatched      = unmatched;
     pthread_mutex_unlock(&g_status_lock);
}

void status_update_rx_stream_clock(int k, long long sliding_ppb, int window_ready)
{
     long long worst = 0;
     int worst_k = 0, ready = 0;

     if (k < 0 || k >= MAX_STREAMS)
         return;
     pthread_mutex_lock(&g_status_lock);
     g_status.streams[k].rx_drift_ppb_sliding = sliding_ppb;
     g_status.streams[k].rx_window_ready = window_ready;
     for (int i = 0; i < g_status.n_streams; i++) {
         if (!g_status.streams[i].rx_window_ready || !g_status.streams[i].rx_pkts_per_s)
             continue;
         if (!ready || llabs(g_status.streams[i].rx_drift_ppb_sliding) > llabs(worst)) {
             worst = g_status.streams[i].rx_drift_ppb_sliding;
             worst_k = i;
         }
         ready = 1;
     }
     g_status.rx_drift_ppb_sliding = worst;
     g_status.rx_drift_stream      = worst_k;
     g_status.rx_sliding_ready     = ready;
     pthread_mutex_unlock(&g_status_lock);
}

void status_update_media_clk(int internal)
{
     pthread_mutex_lock(&g_status_lock);
     g_status.media_clk_internal = internal;
     pthread_mutex_unlock(&g_status_lock);
}

void status_update_conns(int idx, int tx_bound, uint64_t tx_peer, int tx_n, const uint64_t *tx_peers,
                         int rx_bound, uint64_t rx_peer, uint64_t rx_sid)
{
    if (idx < 0 || idx >= g_num_streams)
        return;
    pthread_mutex_lock(&g_status_lock);
    g_status.conn[idx].tx_bound = tx_bound;
    g_status.conn[idx].tx_peer  = tx_peer;
    g_status.conn[idx].tx_n     = tx_n;
    for (int i = 0; i < STATUS_TX_PEERS; i++)
        g_status.conn[idx].tx_peers[i] = i < tx_n ? tx_peers[i] : 0;
    g_status.conn[idx].rx_bound = rx_bound;
    g_status.conn[idx].rx_peer  = rx_peer;
    g_status.conn[idx].rx_sid   = rx_sid;
    pthread_mutex_unlock(&g_status_lock);
}

void status_update_available_index(uint32_t idx)
{
     pthread_mutex_lock(&g_status_lock);
     g_status.available_index = idx;
     pthread_mutex_unlock(&g_status_lock);
}

void status_update_filter_ids(void)
{
     static uint64_t prev_pkts[MAX_STREAMS];
     static uint64_t prev_ms;
     uint64_t now = get_time_in_ms();
     uint64_t dt = prev_ms && now > prev_ms ? now - prev_ms : 0;
     FILE *f = fopen("/proc/snd_igb_avb/streams", "r");
     if (!f) {
         static int warned;
         if (!warned) {
             avb_log(L_SYS, LOG_WARNING, "cannot open /proc/snd_igb_avb/streams (%s)",
                     strerror(errno));
             warned = 1;
         }
         return;
     }

     pthread_mutex_lock(&g_status_lock);
     char line[384];
     int k = 0;

     while (fgets(line, sizeof(line), f) && k < MAX_STREAMS) {
         unsigned src[6], dst[6];
         int filter_id, align;
         unsigned long long pkts, drops, late;

         if (strncmp(line, "  rx: expected_sid=", 19))
             continue;
         if (sscanf(line, "  rx: expected_sid=%*x src_mac=%x:%x:%x:%x:%x:%x dst_mac=%x:%x:%x:%x:%x:%x"
                          " filter=%d slice ch%*d-%*d rx_pkts=%llu rx_drops=%llu align=%d late=%llu",
                    &src[0], &src[1], &src[2], &src[3], &src[4], &src[5],
                    &dst[0], &dst[1], &dst[2], &dst[3], &dst[4], &dst[5],
                    &filter_id, &pkts, &drops, &align, &late) == 17) {
             struct avb_status_stream *st = &g_status.streams[k];

             for (int j = 0; j < 6; j++) {
                 st->rx_src_mac[j] = (uint8_t)src[j];
                 st->rx_dst_mac[j] = (uint8_t)dst[j];
             }
             st->rx_filter_id = filter_id;
             st->rx_drops = drops;
             st->rx_late = late;
             st->align_offset_frames = align;
             st->rx_pkts_per_s = dt && pkts >= prev_pkts[k]
                                 ? (uint32_t)((pkts - prev_pkts[k]) * 1000ULL / dt) : 0;
             st->rx_pkts = pkts;
             prev_pkts[k] = pkts;
         }
         k++;
     }
     pthread_mutex_unlock(&g_status_lock);
     prev_ms = now;
     fclose(f);
}

void status_update_streams(int n, struct avb_stream *streams_out, struct avb_stream *streams_in)
{
     int i;
     int count = n;

     (void)streams_in;

     if (count < 0)
         count = 0;
     if (count > MAX_STREAMS)
         count = MAX_STREAMS;

     pthread_mutex_lock(&g_status_lock);
     g_status.n_streams = count;
     for (i = 0; i < count; i++) {
         g_status.streams[i].stream_id = streams_out[i].stream_id;
         memcpy(g_status.streams[i].dest_mac, streams_out[i].dest_mac, 6);
         g_status.streams[i].dir = 'T';
         g_status.streams[i].rx_filter_id = -1;
     }
     pthread_mutex_unlock(&g_status_lock);
}

void status_update_cbs_bw(uint64_t cbs_bps, uint64_t reserved_bps, int match)
{
     pthread_mutex_lock(&g_status_lock);
     g_status.bw_cbs_programmed_bps = cbs_bps;
     g_status.bw_reserved_bps = reserved_bps;
     g_status.bw_match = match;
     pthread_mutex_unlock(&g_status_lock);
}
