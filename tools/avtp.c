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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

#include <endian.h>

#include "avtp.h"
#include "avb_log.h"
#include "utils.h"
#include "avdecc_aecp.h"
#include "avdecc_acmp.h"
#include "avdecc_peers.h"
#include <jdksavdecc.h>

extern volatile sig_atomic_t running;

int avtp_ifindex = 0;
char avtp_dmac[6] = { 0x91, 0xe0, 0xf0, 0x01, 0x00, 0x00 };
int avtp_sock = -1;

int avtp_socket_create(const char *iface, struct sockaddr *mac)
{
    int sock;
    sock = socket(AF_PACKET, SOCK_RAW, htons(0x22f0));

    if (sock < 0)
    {
        avb_log(L_SYS, LOG_ERR, "raw AVTP socket creation failed (%s)", strerror(errno));
        return -1;
    }

    int temp = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &temp, sizeof(int)) < 0)
    {
        avb_log(L_SYS, LOG_ERR, "could not set SO_REUSEADDR (%s)", strerror(errno));
        close(sock);
        return -1;
    }

    struct ifreq ifreq_i;

    memset(&ifreq_i, 0, sizeof(ifreq_i));
    strncpy(ifreq_i.ifr_name, iface, IFNAMSIZ-1);

    if (ioctl(sock, SIOCGIFINDEX, &ifreq_i) < 0)
    {
        avb_log(L_SYS, LOG_ERR, "SIOCGIFINDEX failed for %s (%s)", iface, strerror(errno));
    }
    avb_log(L_SYS, LOG_DEBUG, "interface %s index=%d", iface, ifreq_i.ifr_ifindex);

    avtp_ifindex = ifreq_i.ifr_ifindex;

    struct sockaddr_ll my_addr;
    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sll_family = AF_PACKET;
    my_addr.sll_protocol = htons(0x22f0);
    my_addr.sll_ifindex = avtp_ifindex;

    if (bind(sock, (struct sockaddr*)&my_addr, sizeof(my_addr)) == -1)
    {
        avb_log(L_SYS, LOG_ERR, "bind of AVTP socket failed (%s)", strerror(errno));
        close(sock);
        return -1;
    }

    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof(struct packet_mreq));
    mreq.mr_ifindex = avtp_ifindex;
    mreq.mr_type = PACKET_MR_MULTICAST;
    mreq.mr_alen = 6;
    memcpy(&mreq.mr_address, avtp_dmac, 6);

    if (setsockopt(sock, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        avb_log(L_SYS, LOG_WARNING, "PACKET_ADD_MEMBERSHIP failed (%s)", strerror(errno));

    struct timeval rcv_timeout = { .tv_sec = 0, .tv_usec = 250000 };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout)) < 0)
        avb_log(L_SYS, LOG_WARNING, "SO_RCVTIMEO failed (%s)", strerror(errno));

    struct ifreq ifreq_c;

    memset(&ifreq_c, 0, sizeof(ifreq_c));
    strncpy(ifreq_c.ifr_name, iface, IFNAMSIZ-1);

    if (ioctl(sock, SIOCGIFHWADDR, &ifreq_c) < 0)
        avb_log(L_SYS, LOG_ERR, "SIOCGIFHWADDR failed (%s)", strerror(errno));

    memcpy(mac, &ifreq_c.ifr_hwaddr, sizeof(ifreq_c.ifr_hwaddr));

    avtp_sock = sock;
    return sock;
}

void avtp_socket_close(void)
{
    if (avtp_sock >= 0)
    {
        close(avtp_sock);
        avtp_sock = -1;
    }
}

int avtp_recv(int sock, char *buffer, int buflen)
{
    struct sockaddr saddr;
    int saddr_len = sizeof(saddr);

    int ret = recvfrom(sock, buffer, buflen, 0, &saddr, (socklen_t *)&saddr_len);

    if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        static uint64_t last_log;
        uint64_t now = get_time_in_ms();
        if (now - last_log > 1000)
        {
            last_log = now;
            avb_log(L_SYS, LOG_WARNING, "recvfrom failed (%s)", strerror(errno));
        }
    }
    return ret;
}

int avtp_send(int sock, const char *buffer, int buflen)
{
    int sendlen;

    struct sockaddr_ll sadr_ll;
    sadr_ll.sll_family = AF_PACKET;
    sadr_ll.sll_ifindex = avtp_ifindex;
    sadr_ll.sll_halen = ETH_ALEN;
    sadr_ll.sll_addr[0] = buffer[0];
    sadr_ll.sll_addr[1] = buffer[1];
    sadr_ll.sll_addr[2] = buffer[2];
    sadr_ll.sll_addr[3] = buffer[3];
    sadr_ll.sll_addr[4] = buffer[4];
    sadr_ll.sll_addr[5] = buffer[5];

    sendlen = sendto(sock, buffer, buflen, 0, (const struct sockaddr*)&sadr_ll, sizeof(struct sockaddr_ll));
    if (sendlen < 0)
    {
        static uint64_t last_log;
        uint64_t now = get_time_in_ms();
        if (now - last_log > 1000)
        {
            last_log = now;
            avb_log(L_SYS, LOG_WARNING, "sendto failed (%s)", strerror(errno));
        }
    }
    return sendlen;
}

void *avtp_message_handler(void *arg)
{
    #define MAX_MSGLEN 1500

    int sock = *(int *)arg;
    int res = 0;
    char *msg = malloc(MAX_MSGLEN);

    if (!msg)
    {
        avb_log(L_SYS, LOG_ERR, "failed to allocate message buffer");
        return NULL;
    }

    while (running)
    {
        res = avtp_recv(sock, msg, MAX_MSGLEN);

        if (res >= 0)
        {
            uint8_t subtype = jdksavdecc_common_control_header_get_subtype(msg, COMMON_HEADER_START);

            switch (subtype)
            {
                case 0x7b:
                {
                    avdecc_aecp_handle_command(sock, msg, res);
                    break;
                }
                case 0x7c:
                {
                    avdecc_acmp_handle_command(sock, msg, res);
                    break;
                }
                case 0x7a:
                {
                    avdecc_peers_handle_adp(msg, res);
                    break;
                }
                case 0x7e:
                case 0:
                default:
                    /* Not handled in user space (kernel relays the data plane) */
                    break;
            }
        }
    }

    free(msg);
    return NULL;
}