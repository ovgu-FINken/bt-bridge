/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011  Nokia Corporation
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <glib.h>


#include "lib/uuid.h"
#include <btio/btio.h>
#include "att.h"
#include "gattrib.h"
#include "gatt.h"
#include "gatttool.h"

#include <termios.h>
#include <unistd.h>
#include <poll.h>

static int ttyfd = STDIN_FILENO;     /* STDIN_FILENO is 0 by default */

static GIOChannel *iochannel = NULL;
static GAttrib *attrib = NULL;
static GMainLoop *event_loop;

static gchar *opt_src = NULL;
static gchar *opt_dst = NULL;
static gchar *opt_dst_type = NULL;
static gchar *opt_sec_level = NULL;
static const int opt_psm = 0;
static int opt_mtu = 0;

char *interface;
int handle;

static enum state {
	STATE_DISCONNECTED=0,
	STATE_CONNECTING=1,
	STATE_CONNECTED=2
} conn_state;

void fatal(char *message)
{
	fprintf(stderr,"fatal error: %s\n",message);
	exit(1);
}

static void send_data(const unsigned char *val, size_t len)
{
  while ( len-- > 0 )
    printf("%c", (char)*val++);
}

static void events_handler(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t evt;
	uint16_t olen;
	size_t plen;

	evt = pdu[0];

	if ( evt != ATT_OP_HANDLE_NOTIFY && evt != ATT_OP_HANDLE_IND )
	{
		printf("#Invalid opcode %02X in event handler??\n", evt);
		return;
	}

	assert( len >= 3 );
	handle = att_get_u16(&pdu[1]);

	send_data( pdu+3, len-3 );
	fflush(stdout);

	if (evt == ATT_OP_HANDLE_NOTIFY)
	{
		return;
	}

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_confirmation(opdu, plen);

	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void connect_cb(GIOChannel *io, GError *err, gpointer user_data)
{
	if (err) {
		conn_state= STATE_DISCONNECTED;
		fflush(stdout);
                printf("# Connect error: %s\n", err->message);
		return;
	}

	attrib = g_attrib_new(iochannel);
	g_attrib_register(attrib, ATT_OP_HANDLE_NOTIFY, GATTRIB_ALL_HANDLES,
						events_handler, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_HANDLE_IND, GATTRIB_ALL_HANDLES,
						events_handler, attrib, NULL);
	conn_state = STATE_CONNECTED;
}

static void disconnect_io()
{
	if (conn_state == STATE_DISCONNECTED)
		return;

	g_attrib_unref(attrib);
	attrib = NULL;
	opt_mtu = 0;

	g_io_channel_shutdown(iochannel, FALSE, NULL);
	g_io_channel_unref(iochannel);
	iochannel = NULL;

	conn_state = STATE_DISCONNECTED;
}

static gboolean channel_watcher(GIOChannel *chan, GIOCondition cond,
				gpointer user_data)
{
	disconnect_io();

	return FALSE;
}

static void cmd_connect(int argcp, char **argvp)
{
	if (conn_state != STATE_DISCONNECTED)
		return;

	if (argcp > 1) {
		g_free(opt_dst);
		opt_dst = g_strdup(argvp[1]);

		g_free(opt_dst_type);
		if (argcp > 2)
			opt_dst_type = g_strdup(argvp[2]);
		else
			opt_dst_type = g_strdup("public");
	}

	if (opt_dst == NULL) {
		fflush(stdout);
		return;
	}

	conn_state = STATE_CONNECTING;
	iochannel = gatt_connect(opt_src, opt_dst, opt_dst_type, opt_sec_level,
						opt_psm, opt_mtu, connect_cb);

	if (iochannel == NULL)
		conn_state = STATE_DISCONNECTED;
	else
		g_io_add_watch(iochannel, G_IO_HUP, channel_watcher, NULL);
}

static void cmd_disconnect(int argcp, char **argvp)
{
	disconnect_io();
}

static int strtohandle(const char *src)
{
	char *e;
	int dst;

	errno = 0;
	dst = strtoll(src, &e, 16);
	if (errno != 0 || *e != '\0')
		return -EINVAL;

	return dst;
}


static gboolean prompt_read(GIOChannel *chan, GIOCondition cond,
							gpointer user_data)
{
	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		g_io_channel_unref(chan);
		return FALSE;
	}
	int bytesread = 0;
	uint8_t c_in;
	bytesread = read(ttyfd, &c_in, 1 /* read up to 1 byte */);
	if(bytesread) {
		gatt_write_char(attrib, handle, &c_in, 1, NULL, NULL);
                fflush(stdout);
	}

	return TRUE;
}



int main(int argc, char *argv[])
{
	if(argc < 3) {
		printf("Need at least 2 arguments: Interface and Handle");
		return 1;
	}

	interface = argv[1];
	handle = strtohandle(argv[2]);


	GIOChannel *pchan;
	gint events;

	opt_sec_level = g_strdup("low");

	opt_src = NULL;
	opt_dst = NULL;
	opt_dst_type = g_strdup("public");

        fflush(stdout);

	event_loop = g_main_loop_new(NULL, FALSE);

	pchan = g_io_channel_unix_new(fileno(stdin));
	g_io_channel_set_close_on_unref(pchan, TRUE);
	events = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
	g_io_add_watch(pchan, events, prompt_read, NULL);
	cmd_connect(2, argv);
	g_main_loop_run(event_loop);

	cmd_disconnect(0, NULL);
        fflush(stdout);
	g_io_channel_unref(pchan);
	g_main_loop_unref(event_loop);

	g_free(opt_src);
	g_free(opt_dst);
	g_free(opt_sec_level);

	return EXIT_SUCCESS;
}

