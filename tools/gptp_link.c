// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#include "gptp_link.h"
#include "avb_log.h"
#include "avdecc_peers.h"
#include "gptp_client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <sys/socket.h>
#include <jdksavdecc_adp.h>

#define ETH_P_GPTP      0x88f7
#define PTP_HDR         14
#define PTP_MIN_LEN     (PTP_HDR + 34)
#define PTP_RESP_LEN    (PTP_HDR + 54)
#define PTP_SYNC        0x0
#define PTP_PDELAY_REQ  0x2
#define PTP_PDELAY_RESP 0x3
#define PTP_ANNOUNCE    0xb

#define EVAL_MS         1000
#define RECENT_MS       3000
#define LINK_HOLD_MS    5000
#define SPLIT_HOLD_MS   15000
#define MAX_RESPONDERS  4

enum { LINK_UNKNOWN, LINK_OK, LINK_DEAF, LINK_NONE, LINK_MULTI };

static int g_sock = -1;
static char g_carrier_path[64];

static struct {
   uint64_t own, partner, logged_partner;
   uint64_t our_req_ms, partner_req_ms, partner_any_ms, resp_ms;
   uint64_t responder[MAX_RESPONDERS], responder_ms[MAX_RESPONDERS];
   uint64_t eval_ms, split_gm;
   int      state, n_resp;
   int      carrier;
   struct avb_diag down, link, split;
} g;

static uint64_t be64(const uint8_t *p)
{
   uint64_t v = 0;
   for (int i = 0; i < 8; i++)
      v = v << 8 | p[i];
   return v;
}

int gptp_link_init(int ifindex)
{
   struct sock_filter code[] = {
      BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_GPTP, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, 0xffff),
      BPF_STMT(BPF_RET | BPF_K, 0),
   };
   struct sock_fprog prog = { .len = sizeof code / sizeof code[0], .filter = code };
   struct sockaddr_ll sll;
   char ifname[IF_NAMESIZE];

   g.carrier = 1;
   if (if_indextoname(ifindex, ifname))
      snprintf(g_carrier_path, sizeof g_carrier_path, "/sys/class/net/%s/carrier", ifname);
   g_sock = socket(AF_PACKET, SOCK_RAW, 0);
   if (g_sock < 0)
      goto fail;
   if (setsockopt(g_sock, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog) < 0)
      goto fail;
   memset(&sll, 0, sizeof sll);
   sll.sll_family = AF_PACKET;
   sll.sll_protocol = htons(ETH_P_ALL);
   sll.sll_ifindex = ifindex;
   if (bind(g_sock, (struct sockaddr *)&sll, sizeof sll) < 0)
      goto fail;
   return 0;

fail:
   avb_log(L_GPTP, LOG_WARNING, "gPTP link watch disabled (%s)", strerror(errno));
   if (g_sock >= 0)
      close(g_sock);
   g_sock = -1;
   return -1;
}

void gptp_link_cleanup(void)
{
   if (g_sock >= 0)
      close(g_sock);
   g_sock = -1;
}

static void note_responder(uint64_t id, uint64_t now)
{
   int i, oldest = 0;

   for (i = 0; i < MAX_RESPONDERS; i++) {
      if (g.responder[i] == id)
         break;
      if (g.responder_ms[i] < g.responder_ms[oldest])
         oldest = i;
   }
   if (i == MAX_RESPONDERS)
      i = oldest;
   g.responder[i] = id;
   g.responder_ms[i] = now;
}

static void note_frame(const uint8_t *f, int len, int outgoing, uint64_t now)
{
   const uint8_t *p = f + PTP_HDR;
   uint64_t src;
   int type;

   if (len < PTP_MIN_LEN)
      return;
   type = p[0] & 0x0f;
   src = be64(p + 20);

   if (outgoing) {
      g.own = src;
      if (type == PTP_PDELAY_REQ)
         g.our_req_ms = now;
      return;
   }
   if (src == g.own)
      return;
   g.partner_any_ms = now;
   if (type == PTP_PDELAY_REQ) {
      g.partner = src;
      g.partner_req_ms = now;
   } else if (type == PTP_PDELAY_RESP && len >= PTP_RESP_LEN && g.own && be64(p + 44) == g.own) {
      g.resp_ms = now;
      note_responder(src, now);
      if (!g.partner)
         g.partner = src;
   } else if ((type == PTP_SYNC || type == PTP_ANNOUNCE) && !g.partner) {
      g.partner = src;
   }
}

static int read_carrier(void)
{
   FILE *f;
   int c = 1;

   if (!g_carrier_path[0] || !(f = fopen(g_carrier_path, "r")))
      return 1;
   if (fscanf(f, "%d", &c) != 1)
      c = 1;
   fclose(f);
   return c;
}

int gptp_link_carrier(void)
{
   return g.carrier;
}

static int recent(uint64_t t, uint64_t now)
{
   return t && now - t <= RECENT_MS;
}

static void eval_link(uint64_t now)
{
   char body[320];
   uint64_t first = 0;

   g.n_resp = 0;
   for (int i = 0; i < MAX_RESPONDERS; i++)
      if (recent(g.responder_ms[i], now)) {
         if (!g.n_resp++)
            first = g.responder[i];
      }

   if (!recent(g.our_req_ms, now))
      g.state = LINK_UNKNOWN;
   else if (g.n_resp > 1)
      g.state = LINK_MULTI;
   else if (recent(g.resp_ms, now))
      g.state = LINK_OK;
   else if (!recent(g.partner_any_ms, now))
      g.state = LINK_NONE;
   else
      g.state = LINK_DEAF;

   if (g.state == LINK_OK && first != g.logged_partner) {
      avb_log(L_GPTP, LOG_INFO, "link partner %016llx answers our Pdelay_Req",
              (unsigned long long)first);
      g.logged_partner = first;
   }

   body[0] = '\0';
   if (g.state == LINK_DEAF)
      snprintf(body, sizeof body,
               "gPTP link: the link partner answers none of our Pdelay_Req: "
               "its gPTP is hung, power-cycle it [%016llx]", (unsigned long long)g.partner);
   else if (g.state == LINK_NONE)
      snprintf(body, sizeof body,
               "gPTP link: no gPTP frames from the link partner: not an AVB bridge, "
               "or its gPTP is down [%016llx]", 0ULL);
   else if (g.state == LINK_MULTI)
      snprintf(body, sizeof body,
               "gPTP link: %d ports answer our Pdelay_Req: a non-AVB switch sits "
               "between us and the AVB bridge [%016llx]", g.n_resp, (unsigned long long)first);
   avb_diag_update(&g.link, body[0] != '\0', now, LINK_HOLD_MS, body);
}

static void eval_split(uint64_t now)
{
   static struct avdecc_peer_info d[AVDECC_PEERS_MAX];
   int n = avdecc_peers_list(d, AVDECC_PEERS_MAX);
   uint64_t ours = gptp_get_grandmaster_id(), gm = 0;
   uint8_t dom = gptp_get_domain_number();
   char body[320] = "";

   for (int i = 0; i < n && !gm; i++) {
      if (d[i].departed || now - d[i].last_adp_ms > d[i].valid_ms)
         continue;
      if (!(d[i].entity_caps & JDKSAVDECC_ADP_ENTITY_CAPABILITY_GPTP_SUPPORTED) || !d[i].gptp_gm)
         continue;
      if (d[i].gptp_gm != ours || d[i].gptp_domain != dom)
         gm = d[i].gptp_gm;
   }

   g.split_gm = gm;
   if (ours && gm && g.carrier)
      snprintf(body, sizeof body,
               "gPTP: devices follow grandmaster %016llx, we follow %016llx%s [%016llx]",
               (unsigned long long)gm, (unsigned long long)ours,
               g.own && ours == g.own ? " (ourselves: the bridge sends us no Announce)" : "",
               (unsigned long long)gm);
   avb_diag_update(&g.split, body[0] != '\0', now, SPLIT_HOLD_MS, body);
}

void gptp_link_poll(uint64_t now_ms)
{
   uint8_t f[128];
   struct sockaddr_ll from;
   socklen_t fl;
   ssize_t len;

   if (g_sock < 0)
      return;
   for (;;) {
      fl = sizeof from;
      len = recvfrom(g_sock, f, sizeof f, MSG_DONTWAIT | MSG_TRUNC, (struct sockaddr *)&from, &fl);
      if (len < 0)
         break;
      if (len > (ssize_t)sizeof f)
         len = sizeof f;
      note_frame(f, (int)len, from.sll_pkttype == PACKET_OUTGOING, now_ms);
   }
   if (now_ms - g.eval_ms < EVAL_MS)
      return;
   g.eval_ms = now_ms;
   g.carrier = read_carrier();
   avb_diag_update(&g.down, !g.carrier, now_ms, 3000,
                   "gPTP link: Ethernet link down (switch rebooting, or cable) [0000000000000000]");
   eval_link(now_ms);
   eval_split(now_ms);
}

int gptp_link_text(char *buf, size_t n)
{
   if (!g.carrier) {
      snprintf(buf, n, "LINK DOWN");
      return 2;
   }
   if (g.link.on && g.state == LINK_DEAF) {
      snprintf(buf, n, "link partner %016llx NOT ANSWERING Pdelay (hung?)",
               (unsigned long long)g.partner);
      return 2;
   }
   if (g.link.on && g.state == LINK_NONE) {
      snprintf(buf, n, "no gPTP link partner");
      return 2;
   }
   if (g.link.on && g.state == LINK_MULTI) {
      snprintf(buf, n, "%d Pdelay responders (non-AVB switch?)", g.n_resp);
      return 2;
   }
   if (g.split.on) {
      snprintf(buf, n, "devices follow GM %016llx", (unsigned long long)g.split_gm);
      return 1;
   }
   if (g.state == LINK_OK) {
      snprintf(buf, n, "link partner %016llx", (unsigned long long)g.logged_partner);
      return 0;
   }
   buf[0] = '\0';
   return 0;
}
