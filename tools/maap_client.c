/******************************************************************************

  Copyright (c) 2012, Intel Corporation
  Copyright (c) 2014, Parrot SA
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

   3. Neither the name of the Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#include <pthread.h>
#include <poll.h>
#include <time.h>

#include <avb_log.h>
#include <maap_client.h>

#define MAAP_PORT_DEFAULT 15364

#define MAAP_DYNAMIC_POOL_BASE 0x91E0F0000000ULL
#define MAAP_DYNAMIC_POOL_SIZE 0xFE00

#define MAAP_INIT_TIMEOUT_MS    2000
#define MAAP_ACQUIRE_TIMEOUT_MS 4000

static int sock_fd;

int send_maap_cmd(Maap_Cmd* cmd)
{
	struct sockaddr_in addr;
	socklen_t addr_len;

	if (sock_fd == -1)
		return -1;
	if (cmd == NULL)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(MAAP_PORT_DEFAULT);
	inet_aton("127.0.0.1", &addr.sin_addr);
	addr_len = sizeof(addr);
	return send(sock_fd, cmd, sizeof(Maap_Cmd), 0);
}

int receive_maap_response(Maap_Notify* response)
{
	struct sockaddr_in addr;
	socklen_t addr_len;

	if (sock_fd == -1)
		return -1;
	if (response == NULL)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(MAAP_PORT_DEFAULT);
	inet_aton("127.0.0.1", &addr.sin_addr);
	addr_len = sizeof(addr);
	return recv(sock_fd, response, sizeof(Maap_Notify), 0);
}

static int wait_for_notify(Maap_Notify* response, int timeout_ms)
{
	struct pollfd pfd;
	int rc;

	if (sock_fd == -1 || response == NULL)
		return -1;

	pfd.fd = sock_fd;
	pfd.events = POLLIN;
	rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0) {
		avb_log(L_AVDECC, LOG_WARNING, "MAAP: poll failed: %s", strerror(errno));
		return -1;
	}
	if (rc == 0)
		return 0;

	rc = receive_maap_response(response);
	if (rc < 0) {
		avb_log(L_AVDECC, LOG_WARNING, "MAAP: failed to receive response");
		return -1;
	}
	if (rc != sizeof(*response)) {
		avb_log(L_AVDECC, LOG_WARNING, "MAAP: short response (%d bytes)", rc);
		return -1;
	}
	return 1;
}

int maap_client_init(struct maap_record* ctx)
{
	struct sockaddr_in addr;
	int rc;

	struct addrinfo hints, *ai, *p;
	int ret;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	if ((ret = getaddrinfo("localhost", "15364", &hints, &ai)) != 0) {
		avb_log(L_AVDECC, LOG_WARNING, "MAAP: getaddrinfo failure %s", gai_strerror(ret));
		return -1;
	}

	for(p = ai; p != NULL; p = p->ai_next) {
                sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if(sock_fd == -1) {
			continue;
		}
		ret = connect(sock_fd, p->ai_addr, p->ai_addrlen);
		if (ret == -1) {
			close(sock_fd);
			continue;
		} else {
			break;
		}
	}

	if (p == NULL) {
		avb_log(L_AVDECC, LOG_WARNING, "MAAP: Unable to connect to the daemon, error %d (%s)", errno, strerror(errno));
		freeaddrinfo(ai);
		return -1;
	}

	freeaddrinfo(ai);

	{
		Maap_Cmd cmd;
		Maap_Notify response;
		int wrc;

		memset(&cmd, 0, sizeof(cmd));
		cmd.kind  = MAAP_CMD_INIT;
		cmd.start = MAAP_DYNAMIC_POOL_BASE;
		cmd.count = MAAP_DYNAMIC_POOL_SIZE;

		if (send_maap_cmd(&cmd) < 0) {
			avb_log(L_AVDECC, LOG_WARNING, "MAAP: failed to send INIT command");
			close(sock_fd); sock_fd = -1;
			return -1;
		}

		wrc = wait_for_notify(&response, MAAP_INIT_TIMEOUT_MS);
		if (wrc <= 0) {
			avb_log(L_AVDECC, LOG_WARNING, "MAAP: no INITIALIZED response from daemon");
			close(sock_fd); sock_fd = -1;
			return -1;
		}
		if (response.kind != MAAP_NOTIFY_INITIALIZED ||
		    (response.result != MAAP_NOTIFY_ERROR_NONE &&
		     response.result != MAAP_NOTIFY_ERROR_ALREADY_INITIALIZED)) {
			avb_log(L_AVDECC, LOG_WARNING,
			        "MAAP: INIT rejected (kind %d, error %d)",
			        response.kind, response.result);
			close(sock_fd); sock_fd = -1;
			return -1;
		}
		avb_log(L_AVDECC, LOG_INFO,
		        "MAAP: daemon pool 0x%012llx size 0x%x",
		        (unsigned long long)response.start, response.count);
	}

	ctx->base = 0;
	if (ctx->count == 0)
		ctx->count = 1;

	return 0;
}

static int ms_until(const struct timespec* deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int)((deadline->tv_sec  - now.tv_sec) * 1000 +
                 (deadline->tv_nsec - now.tv_nsec) / 1000000L);
}

int maap_acquire_adresses(struct maap_record* ctx)
{
    Maap_Cmd cmd;
    Maap_Notify response;
    struct timespec deadline;

    if (sock_fd == -1 || ctx == NULL)
        return -1;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = MAAP_CMD_RESERVE;
    cmd.count = ctx->count;

    if (send_maap_cmd(&cmd) < 0) {
        avb_log(L_AVDECC, LOG_WARNING, "MAAP: failed to send RESERVE command");
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec  +=  MAAP_ACQUIRE_TIMEOUT_MS / 1000;
    deadline.tv_nsec += (MAAP_ACQUIRE_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    for (;;) {
        int remaining = ms_until(&deadline);
        int wrc;

        if (remaining <= 0) {
            avb_log(L_AVDECC, LOG_WARNING, "MAAP: timeout waiting for ACQUIRED");
            return -1;
        }

        wrc = wait_for_notify(&response, remaining);
        if (wrc < 0)
            return -1;
        if (wrc == 0) {
            avb_log(L_AVDECC, LOG_WARNING, "MAAP: timeout waiting for ACQUIRED");
            return -1;
        }

        if (response.kind == MAAP_NOTIFY_ACQUIRING)
            continue;
        if (response.kind != MAAP_NOTIFY_ACQUIRED)
            continue;

        if (response.result == MAAP_NOTIFY_ERROR_RESERVE_NOT_AVAILABLE) {
            avb_log(L_AVDECC, LOG_WARNING, "MAAP: reservation not available");
            return -1;
        }
        if (response.result != MAAP_NOTIFY_ERROR_NONE) {
            avb_log(L_AVDECC, LOG_WARNING, "MAAP: reserve failed with error %d", response.result);
            return -1;
        }

        ctx->base  = response.start;
        ctx->count = response.count;
        ctx->id    = response.id;
        avb_log(L_AVDECC, LOG_INFO, "MAAP: acquired 0x%012llx (n=%u, id=%d)",
                (unsigned long long)ctx->base, ctx->count, ctx->id);
        return 0;
    }
}

int maap_release_adresses(struct maap_record* ctx)
{
    Maap_Cmd cmd;
    Maap_Notify response;
    struct pollfd pfd;
    int rc;

    if (sock_fd == -1 || ctx == NULL)
        return -1;

    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = MAAP_CMD_RELEASE;
    cmd.id = ctx->id;

    rc = send_maap_cmd(&cmd);
    if (rc < 0) {
        avb_log(L_AVDECC, LOG_WARNING, "MAAP: failed to send RELEASE command");
        return -1;
    }

    pfd.fd = sock_fd;
    pfd.events = POLLIN;
    rc = poll(&pfd, 1, 2000);
    if (rc < 0) {
        avb_log(L_AVDECC, LOG_WARNING, "MAAP: poll failed: %s", strerror(errno));
        return -1;
    }
    if (rc == 0) {
        avb_log(L_AVDECC, LOG_WARNING, "MAAP: timeout waiting for RELEASED notification");
        return -1;
    }

    rc = receive_maap_response(&response);
    if (rc < 0) {
        avb_log(L_AVDECC, LOG_WARNING, "MAAP: failed to receive response");
        return -1;
    }

    if (response.kind != MAAP_NOTIFY_RELEASED) {
        avb_log(L_AVDECC, LOG_WARNING, "MAAP: expected RELEASED, got %d", response.kind);
        return -1;
    }

    if (response.result != MAAP_NOTIFY_ERROR_NONE) {
        avb_log(L_AVDECC, LOG_WARNING, "MAAP: release failed with error %d", response.result);
        return -1;
    }

    return 0;
}