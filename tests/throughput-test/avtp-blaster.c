// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
/*
 * avtp-blaster.c -- AVTP load source for the AVB throughput-test tool.
 *
 * Raw-socket (AF_PACKET) AVTP/IEC-61883 blaster. Emits N synthetic AVB talker
 * streams at a fixed per-stream packet rate, with no real audio, purely to load
 * an i210 listener's RX-q1 and find out how many listener streams it can sink
 * with zero drops. Run on a second box (or a second NIC), cabled directly to
 * the i210 under test.
 *
 * Each stream models one real AVB listener: 8 channels @ 48 kHz = 6 samples /
 * packet = 8000 packets/sec. So "--streams N --pps 8000" loads the i210 as if N
 * MOTU-style 8-channel talkers were streaming to it.
 *
 * Frame layout mirrors the snd_igb_avb talker (src/avpdu.c AVPDU_HEADER): the
 * bytes the i210 broad flex filter matches are at offset 12-17:
 *     81 00 60 02   22 f0      (802.1Q VLAN tag + AVTP ethertype 0x22f0)
 * Get those wrong and the i210 silently routes the frames to its default queue
 * instead of RX-q1, and the tool sees nothing.
 *
 * Stream-id scheme: bytes 0-6 = a constant base (src MAC + 0x00), byte 7 = the
 * stream index 0..N-1. The kernel side (avb-bench.c) classifies on that low
 * byte, so the per-stream histogram in /sys/kernel/debug/snd_igb_avb/rx_bench
 * fills evenly -- a quick confirmation every stream is being received.
 *
 * Build:  make            (or: gcc -O2 -Wall -o avtp-blaster avtp-blaster.c)
 * Run:    sudo ./avtp-blaster -i eth0                 # 16 streams x 8000 pps
 *         sudo ./avtp-blaster -i eth0 --streams 32 --pps 8000 --secs 60
 *         sudo ./avtp-blaster -i eth0 --blast         # max rate, find the ceiling
 *
 * Normally you drive this through ../measure-listeners.sh, which sweeps the
 * stream count and reports the ceiling; run it by hand for ad-hoc tests.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#define MAX_STREAMS  64
#define CHANNELS     8
#define SPP          6
#define AUDIO_BYTES  (CHANNELS * SPP * 4)

#pragma pack(push, 1)
struct avpdu {
	uint8_t  dest[6];
	uint8_t  src[6];
	uint32_t vlan_tag;
	uint16_t avtp_vlan_tag;
	uint8_t  subtype;
	uint8_t  flags1;
	uint8_t  seq;
	uint8_t  flags2;
	uint8_t  stream_id[8];
	uint32_t timestamp;
	uint32_t gateway_info;
	uint16_t stream_data_length;
	uint8_t  cip_0, cip_1, cip_2, data_block_size;
	uint8_t  cip_4, data_block_continuity, cip_6, fdf;
	uint16_t syt;
	uint8_t  audio[AUDIO_BYTES];
};
#pragma pack(pop)

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

int main(int argc, char **argv)
{
	const char *iface = NULL;
	int streams = 16;
	int pps = 8000;
	int secs = 0;
	int blast = 0;

	static struct option opts[] = {
		{ "iface",   required_argument, 0, 'i' },
		{ "streams", required_argument, 0, 's' },
		{ "pps",     required_argument, 0, 'p' },
		{ "secs",    required_argument, 0, 't' },
		{ "blast",   no_argument,       0, 'b' },
		{ 0, 0, 0, 0 }
	};
	int c;
	while ((c = getopt_long(argc, argv, "i:s:p:t:b", opts, NULL)) != -1) {
		switch (c) {
		case 'i': iface = optarg; break;
		case 's': streams = atoi(optarg); break;
		case 'p': pps = atoi(optarg); break;
		case 't': secs = atoi(optarg); break;
		case 'b': blast = 1; break;
		default:
			fprintf(stderr, "usage: %s -i <iface> [--streams 1..%d] [--pps P] "
				"[--secs S] [--blast]\n", argv[0], MAX_STREAMS);
			return 2;
		}
	}
	if (!iface || streams < 1 || streams > MAX_STREAMS || pps < 1) {
		fprintf(stderr, "usage: %s -i <iface> [--streams 1..%d] [--pps P] "
			"[--secs S] [--blast]\n", argv[0], MAX_STREAMS);
		return 2;
	}

	int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) { perror("socket(AF_PACKET) -- need root/CAP_NET_RAW"); return 1; }

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); return 1; }
	int ifindex = ifr.ifr_ifindex;
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) { perror("SIOCGIFHWADDR"); return 1; }
	uint8_t srcmac[6];
	memcpy(srcmac, ifr.ifr_hwaddr.sa_data, 6);

	uint8_t dstmac[6] = { 0x91, 0xe0, 0xf0, 0x00, 0xfe, 0x00 };

	struct avpdu frame[MAX_STREAMS];
	struct sockaddr_ll addr[MAX_STREAMS];
	struct iovec iov[MAX_STREAMS];
	struct mmsghdr msgs[MAX_STREAMS];
	memset(frame, 0, sizeof(frame));
	memset(addr, 0, sizeof(addr));
	memset(iov, 0, sizeof(iov));
	memset(msgs, 0, sizeof(msgs));

	for (int s = 0; s < streams; s++) {
		struct avpdu *f = &frame[s];
		memcpy(f->dest, dstmac, 6);
		memcpy(f->src, srcmac, 6);
		/* bytes 12-17 must match the i210 RX-q1 flex filter, else frames land in the default queue */
		f->vlan_tag      = htonl(0x81006002);
		f->avtp_vlan_tag = htons(0x22f0);
		f->subtype       = 0x00;
		f->flags1        = 0x80;
		f->seq           = 0;
		f->flags2        = 0x00;
		memcpy(f->stream_id, srcmac, 6);
		f->stream_id[6]  = 0x00;
		f->stream_id[7]  = (uint8_t)s;
		f->timestamp     = 0;
		f->gateway_info  = 0;
		f->stream_data_length = htons(AUDIO_BYTES + 8);
		f->cip_0 = 0x5f; f->cip_1 = 0xa0; f->cip_2 = 0x3f;
		f->data_block_size = CHANNELS;
		f->cip_4 = 0x00; f->data_block_continuity = 0; f->cip_6 = 0x90;
		f->fdf   = 2;
		f->syt   = htons(0x08);

		addr[s].sll_family  = AF_PACKET;
		addr[s].sll_ifindex = ifindex;
		addr[s].sll_halen   = 6;
		memcpy(addr[s].sll_addr, dstmac, 6);

		iov[s].iov_base = f;
		iov[s].iov_len  = sizeof(*f);
		msgs[s].msg_hdr.msg_name    = &addr[s];
		msgs[s].msg_hdr.msg_namelen = sizeof(addr[s]);
		msgs[s].msg_hdr.msg_iov     = &iov[s];
		msgs[s].msg_hdr.msg_iovlen  = 1;
	}

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	uint64_t period_ns = 1000000000ull / (uint64_t)pps;
	uint64_t start = now_ns();
	uint64_t deadline = start;
	uint64_t sent = 0, last_sent = 0, last_report = start;

	printf("avtp-blaster: iface=%s ifindex=%d streams=%d pps/stream=%d "
	       "total=%d pps frame=%zuB %s\n",
	       iface, ifindex, streams, pps, streams * pps, sizeof(struct avpdu),
	       blast ? "[BLAST/max-rate]" : "");
	printf("  src=%02x:%02x:%02x:%02x:%02x:%02x  dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
	       srcmac[0], srcmac[1], srcmac[2], srcmac[3], srcmac[4], srcmac[5],
	       dstmac[0], dstmac[1], dstmac[2], dstmac[3], dstmac[4], dstmac[5]);

	uint8_t dbc = 0;
	while (running) {
		for (int s = 0; s < streams; s++) {
			frame[s].seq++;
			frame[s].data_block_continuity = dbc;
			frame[s].timestamp = htonl((uint32_t)(now_ns() & 0xffffffff));
		}
		dbc = (uint8_t)(dbc + SPP);

		int n = sendmmsg(fd, msgs, streams, 0);
		if (n < 0) {
			if (errno == EINTR) break;
			if (errno == ENOBUFS || errno == EAGAIN) { /* TX backpressure */ }
			else { perror("sendmmsg"); break; }
		} else {
			sent += (uint64_t)n;
		}

		if (!blast) {
			deadline += period_ns;
			struct timespec ts = {
				.tv_sec  = (time_t)(deadline / 1000000000ull),
				.tv_nsec = (long)(deadline % 1000000000ull),
			};
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		}

		uint64_t t = now_ns();
		if (t - last_report >= 1000000000ull) {
			double dt = (t - last_report) / 1e9;
			printf("  sent=%llu  (%.0f pps)\n",
			       (unsigned long long)sent, (sent - last_sent) / dt);
			fflush(stdout);
			last_sent = sent;
			last_report = t;
		}
		if (secs && (t - start) >= (uint64_t)secs * 1000000000ull)
			break;
	}

	double total_s = (now_ns() - start) / 1e9;
	printf("avtp-blaster: done. sent=%llu in %.2fs (avg %.0f pps)\n",
	       (unsigned long long)sent, total_s, sent / (total_s > 0 ? total_s : 1));
	close(fd);
	return 0;
}