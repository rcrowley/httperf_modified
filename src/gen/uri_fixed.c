/*
    httperf -- a tool for measuring web server performance
    Copyright 2000-2007 Hewlett-Packard Company and Contributors listed in
    AUTHORS file. Originally contributed by David Mosberger-Tang

    This file is part of httperf, a web server performance measurment
    tool.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.
    
    In addition, as a special exception, the copyright holders give
    permission to link the code of this work with the OpenSSL project's
    "OpenSSL" library (or with modified versions of it that use the same
    license as the "OpenSSL" library), and distribute linked combinations
    including the two.  You must obey the GNU General Public License in
    all respects for all of the code used other than "OpenSSL".  If you
    modify this file, you may extend this exception to your version of the
    file, but you are not obligated to do so.  If you do not wish to do
    so, delete this exception statement from your version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  
    02110-1301, USA
*/

/* Causes calls to make a request to the fixed URI specified by
   PARAM.URI.  */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <httperf.h>
#include <call.h>
#include <event.h>

static char uri[400];
static size_t uri_len;
static char uri_html[400]; /* for the --random-extension option */
static size_t uri_html_len;
static char uri_gif[400];
static size_t uri_gif_len;

/* "randomized" URI chars... put random digit */
static int numrandloc = 0;
static int randloc[20];

static void
set_uri (Event_Type et, Call *call)
{
  int i;
  char *this_uri = uri;
  int this_len = uri_len;

  assert (et == EV_CALL_NEW && object_is_call (call));

  if (param.random_extension) {
      if (rand() % 1000 < param.random_extension) {
          this_uri = uri_html;
          this_len = uri_html_len;
      }
      else {
          this_uri = uri_gif;
          this_len = uri_gif_len;
      }
  }

  for(i = 0; i < numrandloc; i++)
      this_uri[randloc[i]]= '0' + (rand() % 10);

 call_set_uri (call, this_uri, this_len);
}

static void
init (void)
{
  Any_Type arg;

  uri_len = strlen (param.uri);
  snprintf(uri,sizeof(uri),"%s",param.uri);
  uri_len = strlen(uri);
  snprintf(uri_html,sizeof(uri),"%s.html",param.uri);
  uri_html_len = strlen(uri_html);
  snprintf(uri_gif,sizeof(uri),"%s.gif",param.uri);
  uri_gif_len = strlen(uri_gif);

  if (param.random_uri) {
      /* find positions containing a $ to replace by digits */
      char *p = uri;
      while ((p = strchr(p,'#')) && numrandloc < 20) {
          randloc[numrandloc++] = p - uri;
          p++;
      }
  }

  arg.l = 0;
  event_register_handler (EV_CALL_NEW, (Event_Handler) set_uri, arg);
}

Load_Generator uri_fixed =
  {
    "fixed url",
    init,
    no_op,
    no_op
  };
