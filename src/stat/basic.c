/*
    httperf -- a tool for measuring web server performance
    Copyright (C) 2000  Hewlett-Packard Company
    Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

    This file is part of httperf, a web server performance measurment
    tool.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
    02111-1307 USA
*/

/* Basic statistics collector.  */

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <float.h>
#include <stdio.h>

#include <httperf.h>
#include <call.h>
#include <event.h>
#include <stats.h>

/* Increase this if it does not cover at least 50% of all response
   times.  */
#define MAX_LIFETIME	100.0		/* max. conn. lifetime in seconds */
#define BIN_WIDTH	1e-3
#define NUM_BINS	((u_int) (MAX_LIFETIME / BIN_WIDTH))

static struct event_statss {
  u_int num;
  Time  sum, sum2, min, max, avg, stddev;
  u_int hist[NUM_BINS];
  Time  perc[101];
  } es_lifetime, es_connect, es_response, es_transfer;

static struct
  {
    u_int num_conns_issued;	/* total # of connections issued */
    u_int num_replies[6];	/* completion count per status class */
    u_int num_client_timeouts;	/* # of client timeouts */
    u_int num_sock_fdunavail;	/* # of times out of filedescriptors */
    u_int num_sock_ftabfull;	/* # of times file table was full */
    u_int num_sock_refused;	/* # of ECONNREFUSED */
    u_int num_sock_reset;	/* # of ECONNRESET */
    u_int num_sock_timeouts;	/* # of ETIMEDOUT */
    u_int num_sock_addrunavail;/* # of EADDRNOTAVAIL */
    u_int num_other_errors;	/* # of other errors */
    u_int max_conns;		/* max # of concurrent connections */

    u_int num_reply_rates;
    Time reply_rate_sum;
    Time reply_rate_sum2;
    Time reply_rate_min;
    Time reply_rate_max;

    u_int num_sent;		/* # of requests sent */
    size_t req_bytes_sent;

    u_int num_received;		/* # of replies received */
    u_wide hdr_bytes_received;	/* sum of all header bytes */
    u_wide reply_bytes_received;	/* sum of all data bytes */
    u_wide footer_bytes_received;	/* sum of all footer bytes */

  }
basic;

static u_int num_active_conns;
static u_int num_replies;	/* # of replies received in this interval */

/* for each event, record elapsed time etc. */
void updatestats (struct event_statss *event, Time t) {
  u_int bin;

  event->num ++;
  event->sum += t;
  event->sum2 += SQUARE(t);
  if (t > event->max) 
    event->max = t;
  if (t < event->min)
    event->min = t;

  bin = t*NUM_BINS/MAX_LIFETIME;
  if (bin >= NUM_BINS) 
    bin = NUM_BINS-1;
  event->hist[bin]++;
  }

/* when done collecting data, calculate average, percentiles, etc */
void calcstats (struct event_statss *event) {
  u_int p, i, n;
  n=0;
  p=1;

  event->avg = AVG(event->sum, event->num);
  event->stddev = STDDEV(event->sum, event->sum2, event->num);

  /* scan histogram, calculating percentiles */  
  for (i = 0; i < NUM_BINS; i++) {
    n += event->hist[i];
    while (n > (event->num * p / 100)) {
      assert(p<=100);
      event->perc[p] = (i + 0.5) * BIN_WIDTH;
      p++;
      }
    }
  }  

void printstats (struct event_statss *event) {
  printf("%6d %6.1f %6.1f %6.1f %4.0f %4.0f %4.0f %4.0f %4.0f %4.0f %4.0f %6.1f",
         event->num,
         1e3 * event->avg,
         1e3 * event->stddev,
         1e3 * event->min,
         1e3 * event->perc[10],
         1e3 * event->perc[25],
         1e3 * event->perc[50],
         1e3 * event->perc[75],
         1e3 * event->perc[90],
         1e3 * event->perc[95],
         1e3 * event->perc[99],
         1e3 * event->max);
  }

void hist_print (u_int *hist) {
  int i;
  for (i = 0; i < NUM_BINS; ++i)
    if (hist[i]) {
      if (i > 0 && hist[i - 1] == 0)
	printf ("%14c\n", ':');
      printf ("%16.1f %u\n", 1e3*(i + 0.5)*BIN_WIDTH  , hist[i]);
      }
  }


static void
perf_sample (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type call_arg)
{
  Time weight = call_arg.d;
  double rate;

  assert (et == EV_PERF_SAMPLE);

  rate = weight*num_replies;

  if (verbose)
    printf ("reply-rate = %-8.1f\n", rate);

  basic.reply_rate_sum += rate;
  basic.reply_rate_sum2 += SQUARE (rate);
  if (rate < basic.reply_rate_min)
    basic.reply_rate_min = rate;
  if (rate > basic.reply_rate_max)
    basic.reply_rate_max = rate;
  ++basic.num_reply_rates;

  /* prepare for next sample interval: */
  num_replies = 0;
}

static void
conn_timeout (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type call_arg)
{
  assert (et == EV_CONN_TIMEOUT);

  ++basic.num_client_timeouts;
}

static void
conn_fail (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type call_arg)
{
  static int first_time = 1;
  int err = call_arg.i;

  assert (et == EV_CONN_FAILED);

  switch (err)
    {
#ifdef __linux__
    case EINVAL:	/* Linux has a strange way of saying "out of fds"... */
#endif
    case EMFILE:	++basic.num_sock_fdunavail; break;
    case ENFILE:	++basic.num_sock_ftabfull; break;
    case ECONNREFUSED:	++basic.num_sock_refused; break;
    case ETIMEDOUT:	++basic.num_sock_timeouts; break;

    case EPIPE:
    case ECONNRESET:
      ++basic.num_sock_reset;
      break;

    default:
      if (first_time)
	{
	  first_time = 0;
	  fprintf (stderr, "%s: connection failed with unexpected error %d\n",
		   prog_name, errno);
	}
      ++basic.num_other_errors;
      break;
    }
}

static void
conn_created (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type c_arg)
{
  ++num_active_conns;
  if (num_active_conns > basic.max_conns)
    basic.max_conns = num_active_conns;
}

static void
conn_connecting (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type c_arg)
{
  Conn *s = (Conn *) obj;

  assert (et == EV_CONN_CONNECTING && object_is_conn (s));

  s->basic.time_connect_start = timer_now ();
  ++basic.num_conns_issued;
}

static void
conn_connected (Event_Type et, Object *obj, Any_Type reg_arg,
		Any_Type call_arg)
{
  Conn *s = (Conn *) obj;

  assert (et == EV_CONN_CONNECTED && object_is_conn (s));

  updatestats (&es_connect, (timer_now () - s->basic.time_connect_start));

}

static void
conn_destroyed (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type c_arg)
{
  Conn *s = (Conn *) obj;

  assert (et == EV_CONN_DESTROYED && object_is_conn (s)
	  && num_active_conns > 0);

  if (s->basic.num_calls_completed > 0)
    updatestats(&es_lifetime, timer_now () - s->basic.time_connect_start);
  --num_active_conns;
}

static void
send_start (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type call_arg)
{
  Call *c = (Call *) obj;

  assert (et == EV_CALL_SEND_START && object_is_call (c));

  c->basic.time_send_start = timer_now ();
}

static void
send_stop (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type call_arg)
{
  Call *c = (Call *) obj;

  assert (et == EV_CALL_SEND_STOP && object_is_call (c));

  basic.req_bytes_sent += c->req.size;
  ++basic.num_sent;
}

static void
recv_start (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type call_arg)
{
  Call *c = (Call *) obj;
  Time now;

  assert (et == EV_CALL_RECV_START && object_is_call (c));

  now = timer_now ();

  updatestats(&es_response, now - c->basic.time_send_start);
  c->basic.time_recv_start = now;

}

static void
recv_stop (Event_Type et, Object *obj, Any_Type reg_arg, Any_Type call_arg)
{
  Call *c = (Call *) obj;
  int index;
  Time xfer_time;

  assert (et == EV_CALL_RECV_STOP && object_is_call (c));
  assert (c->basic.time_recv_start > 0);

  xfer_time = timer_now () - c->basic.time_recv_start;

  updatestats(&es_transfer, xfer_time);

  basic.hdr_bytes_received += c->reply.header_bytes;
  basic.reply_bytes_received += c->reply.content_bytes;
  basic.footer_bytes_received += c->reply.footer_bytes;

  index = (c->reply.status / 100);
  assert ((unsigned) index < NELEMS (basic.num_replies));
  ++basic.num_replies[index];
  ++num_replies;

  ++c->conn->basic.num_calls_completed;
}

static void
init (void)
{
  Any_Type arg;

  es_lifetime.min = DBL_MAX; 
  es_response.min = DBL_MAX;
  es_connect.min = DBL_MAX;
  es_transfer.min = DBL_MAX;

  arg.l = 0;
  event_register_handler (EV_PERF_SAMPLE, perf_sample, arg);
  event_register_handler (EV_CONN_FAILED, conn_fail, arg);
  event_register_handler (EV_CONN_TIMEOUT, conn_timeout, arg);
  event_register_handler (EV_CONN_NEW, conn_created, arg);
  event_register_handler (EV_CONN_CONNECTING, conn_connecting, arg);
  event_register_handler (EV_CONN_CONNECTED, conn_connected, arg);
  event_register_handler (EV_CONN_DESTROYED, conn_destroyed, arg);
  event_register_handler (EV_CALL_SEND_START, send_start, arg);
  event_register_handler (EV_CALL_SEND_STOP, send_stop, arg);
  event_register_handler (EV_CALL_RECV_START, recv_start, arg);
  event_register_handler (EV_CALL_RECV_STOP, recv_stop, arg);
}

static void
dump (void)
{
  Time conn_period = 0.0, call_period = 0.0;
  Time call_size = 0.0, hdr_size = 0.0, reply_size = 0.0, footer_size = 0.0;
  double reply_rate_avg = 0.0, reply_rate_stddev = 0.0;
  int i, total_replies = 0;
  Time delta, user, sys;
  u_wide total_size;

  calcstats(&es_lifetime);
  calcstats(&es_connect);
  calcstats(&es_response);
  calcstats(&es_transfer);

  for (i = 1; i < NELEMS (basic.num_replies); ++i)
    total_replies += basic.num_replies[i];

  delta = test_time_stop - test_time_start;


  if (percentiles) {
    printf("\nPercentiles [ms] - lifetime  connect  response  transfer:\n");
    for (i=1;i<=100;i++)
      printf("%6d%% %6.0f %6.0f %6.0f %6.0f\n",
             i,
	     1e3 * es_lifetime.perc[i],
             1e3 * es_connect.perc[i],
             1e3 * es_response.perc[i],
             1e3 * es_transfer.perc[i]);
  }
       
  /* One line summary output with --summary */
  if (summary) {
    printf ("%8.1f ", 1e3 * es_connect.avg);
    if (es_response.num != es_connect.num) 
      printf("******");
    else 
      printf ("%6.1f ", 1e3 * es_response.avg);
    if (total_replies != es_connect.num)
      printf("******");
    else 
      printf ("%6.1f ", 1e3 * es_transfer.avg);
    if (es_lifetime.num != es_connect.num)
      printf("******");
    else 
      printf ("%6.1f ", 1e3 * es_lifetime.avg);
    if (total_replies != es_connect.num)
      printf("   * * *   * * * * *"); 
    else {
      hdr_size = basic.hdr_bytes_received / total_replies;
      reply_size = basic.reply_bytes_received / total_replies;
      footer_size = basic.footer_bytes_received / total_replies;
      printf ("   %d %d %d",(int) hdr_size, (int) reply_size, (int) footer_size);
      printf ("   %d %d %d %d %d",
	      basic.num_replies[1], 
	      basic.num_replies[2], 
	      basic.num_replies[3],
	      basic.num_replies[4], 
	      basic.num_replies[5]);
    }
    printf("\n");
    return;
  }

  if (verbose > 1)
    {
      printf ("\nHistogram - connection lifetime (ms):\n");
      hist_print(es_lifetime.hist);
      printf ("\nHistogram - connect time (ms):\n");
      hist_print(es_connect.hist);
      printf ("\nHistogram - response time (ms):\n");
      hist_print(es_response.hist);
      printf ("\nHistogram - transfer time (ms):\n");
      hist_print(es_transfer.hist);
    }

  printf ("\nTotal: connections %u requests %u replies %u "
	  "test-duration %.3f s\n",
	  basic.num_conns_issued, basic.num_sent, total_replies,
	  delta);

  putchar ('\n');

  if (basic.num_conns_issued)
    conn_period = delta/basic.num_conns_issued;
  printf ("Connection rate: %.1f conn/s (%.1f ms/conn, "
	  "<=%u concurrent connections)\n",
	  basic.num_conns_issued / delta, 1e3*conn_period, basic.max_conns);

  printf ("Connection length [replies/conn]: %.3f\n",
	  es_lifetime.num > 0
	  ? total_replies/ (double) es_lifetime.num : 0.0);

  printf("\n                       num    avg stddev    min  10%%  25%%  50%%  75%%  90%%  95%%  99%% max");
  printf("\nConnection life [ms]");
  printstats(&es_lifetime);
  printf("\nHandshake time  [ms]");
  printstats(&es_connect);
  printf("\nResponse time   [ms]");
  printstats(&es_response);
  printf("\nTransfer time   [ms]");
  printstats(&es_transfer);
  printf("\n\n");

  if (basic.num_sent > 0)
    call_period = delta/basic.num_sent;
  printf ("Request rate: %.1f req/s (%.1f ms/req)\n",
	  basic.num_sent / delta, 1e3*call_period);

  if (basic.num_sent)
    call_size = basic.req_bytes_sent / basic.num_sent;
  printf ("Request size [B]: %.1f\n", call_size);

  putchar ('\n');

  if (basic.num_reply_rates > 0)
    {
      reply_rate_avg = (basic.reply_rate_sum / basic.num_reply_rates);
      if (basic.num_reply_rates > 1)
	reply_rate_stddev = STDDEV (basic.reply_rate_sum,
				    basic.reply_rate_sum2,
				    basic.num_reply_rates);
    }
  printf ("Reply rate [replies/s]: min %.1f avg %.1f max %.1f stddev %.1f "
	  "(%u samples)\n",
	  basic.num_reply_rates > 0 ? basic.reply_rate_min : 0.0,
	  reply_rate_avg, basic.reply_rate_max,
	  reply_rate_stddev, basic.num_reply_rates);

  if (total_replies)
    {
      hdr_size = basic.hdr_bytes_received / total_replies;
      reply_size = basic.reply_bytes_received / total_replies;
      footer_size = basic.footer_bytes_received / total_replies;
    }
  printf ("Reply size [B]: header %.1f content %.1f footer %.1f "
	  "(total %.1f)\n", hdr_size, reply_size, footer_size,
	  hdr_size + reply_size + footer_size);

  printf ("Reply status: 1xx=%u 2xx=%u 3xx=%u 4xx=%u 5xx=%u\n",
	  basic.num_replies[1], basic.num_replies[2], basic.num_replies[3],
	  basic.num_replies[4], basic.num_replies[5]);

  putchar ('\n');

  user = (TV_TO_SEC (test_rusage_stop.ru_utime)
	  - TV_TO_SEC (test_rusage_start.ru_utime));
  sys = (TV_TO_SEC (test_rusage_stop.ru_stime)
	  - TV_TO_SEC (test_rusage_start.ru_stime));
  printf ("CPU time [s]: user %.2f system %.2f (user %.1f%% system %.1f%% "
	  "total %.1f%%)\n", user, sys, 100.0*user/delta, 100.0*sys/delta,
	  100.0*(user + sys)/delta);

  total_size = (basic.req_bytes_sent
		+ basic.hdr_bytes_received + basic.reply_bytes_received);
  printf ("Net I/O: %.1f KB/s (%.1f*10^6 bps)\n",
	  total_size/delta / 1024.0, 8e-6*total_size/delta);

  putchar ('\n');

  printf ("Errors: total %u client-timo %u socket-timo %u "
	  "connrefused %u connreset %u\n"
	  "Errors: fd-unavail %u addrunavail %u ftab-full %u other %u\n",
	  (basic.num_client_timeouts + basic.num_sock_timeouts
	   + basic.num_sock_fdunavail + basic.num_sock_ftabfull
	   + basic.num_sock_refused + basic.num_sock_reset
	   + basic.num_sock_addrunavail + basic.num_other_errors),
	  basic.num_client_timeouts, basic.num_sock_timeouts,
	  basic.num_sock_refused, basic.num_sock_reset,
	  basic.num_sock_fdunavail, basic.num_sock_addrunavail,
	  basic.num_sock_ftabfull, basic.num_other_errors);
}

Stat_Collector stats_basic =
  {
    "Basic statistics",
    init,
    no_op,
    no_op,
    dump
  };
