// SPDX-License-Identifier: BSD-2-Clause
// SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#include "avb_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

const char *avb_layer_tag(enum avb_layer layer)
{
   switch (layer)
   {
      case L_GPTP:   return "gPTP";
      case L_MSRP:   return "MSRP";
      case L_AVTP:   return "AVTP";
      case L_AVDECC: return "AVDECC";
      case L_FQTSS:  return "FQTSS";
      case L_DIAG:   return "DIAG";
      default:       return "sys";
   }
}

static pthread_mutex_t s_last_lock = PTHREAD_MUTEX_INITIALIZER;
static char            s_last_msg[256];
static time_t          s_last_time;

void avb_log_init(void)
{
   openlog("avb-user", LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

void avb_log_close(void)
{
   closelog();
}

void avb_log(enum avb_layer layer, int level, const char *fmt, ...)
{
   char    body[400];
   va_list ap;

   va_start(ap, fmt);
   vsnprintf(body, sizeof(body), fmt, ap);
   va_end(ap);

   syslog(level, "[%s] %s", avb_layer_tag(layer), body);

   size_t n = strlen(body);
   while (n && (body[n - 1] == '\n' || body[n - 1] == '\r' || body[n - 1] == ' '))
      body[--n] = '\0';
   for (char *c = body; *c; c++)
      if ((unsigned char)*c < 0x20)
         *c = ' ';

   pthread_mutex_lock(&s_last_lock);
   snprintf(s_last_msg, sizeof(s_last_msg), "[%s] %s", avb_layer_tag(layer), body);
   s_last_time = time(NULL);
   pthread_mutex_unlock(&s_last_lock);
}

void avb_log_last(char *buf, size_t buflen)
{
   char      ts[16] = "";
   struct tm tm;

   pthread_mutex_lock(&s_last_lock);
   if (s_last_time)
   {
      localtime_r(&s_last_time, &tm);
      strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
      snprintf(buf, buflen, "%s  (%s)", s_last_msg, ts);
   }
   else
   {
      snprintf(buf, buflen, "%s", "(no events yet)");
   }
   pthread_mutex_unlock(&s_last_lock);
}

void avb_diag_update(struct avb_diag *d, int cond, uint64_t now_ms, uint64_t hold_ms,
                     const char *body)
{
   if (!cond) {
      if (d->on)
         avb_log(L_DIAG, LOG_INFO, "cleared: %s", d->body);
      d->on = 0;
      d->since_ms = 0;
      return;
   }
   if (!d->since_ms)
      d->since_ms = now_ms;
   if (d->on && strcmp(d->body, body)) {
      avb_log(L_DIAG, LOG_INFO, "cleared: %s", d->body);
      d->on = 0;
   }
   if (!d->on && now_ms - d->since_ms >= hold_ms) {
      snprintf(d->body, sizeof d->body, "%s", body);
      d->on = 1;
      avb_log(L_DIAG, LOG_WARNING, "%s", d->body);
   }
}
