/*
 * Copyright (c) 2008-2016, The Linux Foundation. All rights reserved.
 * Copyright (c) 2016, Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "crc_ccitt.h"
#include "diag.h"
#include "diag_cntl.h"
#include "list.h"
#include "mbuf.h"
#include "peripheral.h"
#include "util.h"
#include "watch.h"

struct list_head diag_cmds = LIST_INIT(diag_cmds);
struct list_head diag_clients = LIST_INIT(diag_clients);

static uint8_t *hdlc_encode(uint8_t *src, size_t slen, size_t *dlen)
{
	uint16_t crc = CRC_16_L_SEED;
	uint8_t tmp[2];
	uint8_t *dst;
	uint8_t *s = src;
	uint8_t *d;
	int i;

	dst = malloc((slen + 2) * 2 + 1);
	if (!dst)
		return NULL;

	d = dst;
	while (s < src + slen) {
		crc = crc_ccitt_byte(crc, *s);

		if (*s == 0x7d || *s == 0x7e) {
			*d++ = 0x7d;
			*d++ = *s++ ^ 0x20;
		} else {
			*d++ = *s++;
		}
	}

	tmp[0] = ~crc & 0xff;
	tmp[1] = ~crc >> 8;

	s = tmp;
	for (i = 0; i < 2; i++) {
		if (*s == 0x7d || *s == 0x7e) {
			*d++ = 0x7d;
			*d++ = *s++ ^ 0x20;
		} else {
			*d++ = *s++;
		}
	}

	*d++ = 0x7e;
	*dlen = d - dst;

	return dst;
}

static uint8_t *hdlc_decode_one(uint8_t **buf, size_t *len, size_t *msglen)
{
	uint8_t *dst = *buf;
	uint8_t *src = *buf;
	uint8_t *msg = *buf;
	uint8_t escape = 0;
	uint8_t ch;

	for (;;) {
		if (src >= *buf + *len)
			return NULL;

		ch = *src++;

		if (ch == 0x7e) {
			break;
		} else if (ch == 0x7d) {
			escape = 0x20;
		} else {
			*dst++ = ch ^ escape;
			escape = 0;
		}
	}

	*len -= src - msg;
	*buf = src;

	dst--;
	dst--;

	*msglen = dst - msg;
	return msg;
}

static int hdlc_enqueue(struct list_head *queue, uint8_t *msg, size_t msglen)
{
	struct mbuf *mbuf;
	uint8_t *outbuf;
	size_t outlen;
	void *ptr;

	outbuf = hdlc_encode(msg, msglen, &outlen);
	if (!outbuf)
		err(1, "failed to allocate hdlc destination buffer");

	mbuf = mbuf_alloc(outlen);
	ptr = mbuf_put(mbuf, outlen);
	memcpy(ptr, outbuf, outlen);

	list_add(queue, &mbuf->node);

	return 0;
}

void queue_push(struct list_head *queue, uint8_t *msg, size_t msglen)
{
	struct mbuf *mbuf;
	void *ptr;

	mbuf = mbuf_alloc(msglen);
	ptr = mbuf_put(mbuf, msglen);
	memcpy(ptr, msg, msglen);

	list_add(queue, &mbuf->node);
}

int diag_data_recv(int fd, void *data)
{
	struct peripheral *peripheral = data;
	struct diag_client *client;
	struct list_head *item;
	uint8_t buf[4096];
	uint8_t *ptr;
	uint8_t *msg;
	size_t msglen;
	size_t len;
	ssize_t n;

	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno != EAGAIN) {
				warn("failed to read from data channel");
				peripheral_close(peripheral);
			}

			break;
		}

		ptr = buf;
		len = n;
		for (;;) {
			if (peripheral->features & DIAG_FEATURE_APPS_HDLC_ENCODE) {
				msg = ptr;
				msglen = len;
			} else {
				msg = hdlc_decode_one(&ptr, &len, &msglen);
				if (!msg)
					break;
			}

			list_for_each(item, &diag_clients) {
				client = container_of(item, struct diag_client, node);

				queue_push(&client->outq, msg, msglen);
			}

			if (peripheral->features & DIAG_FEATURE_APPS_HDLC_ENCODE)
				break;
		}
	}

	return 0;
}

static int diag_sock_connect(const char *hostname, unsigned short port)
{
	struct sockaddr_in addr;
	struct hostent *host;
	int ret;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	host = gethostbyname(hostname);
	if (!host)
		return -errno;

	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	bcopy(host->h_addr, &addr.sin_addr.s_addr, host->h_length);
	addr.sin_port = htons(port);

	ret = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0)
		return -errno;

	ret = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (ret < 0)
		return -errno;

	printf("Connected to %s:%d\n", hostname, port);

	return fd;
}

static int diag_cmd_dispatch(uint8_t *ptr, size_t len)
{
	struct peripheral *peripheral;
	struct list_head *item;
	struct diag_cmd *dc;
	uint8_t *outbuf;
	size_t outlen;
	unsigned int key;
	int handled = 0;

	outbuf = hdlc_encode(ptr, len, &outlen);
	if (!outbuf)
		err(1, "failed to allocate hdlc destination buffer");

	if (ptr[0] == DIAG_CMD_SUBSYS_DISPATCH)
		key = ptr[0] << 24 | ptr[1] << 16 | ptr[3] << 8 | ptr[2];
	else
		key = 0xff << 24 | 0xff << 16 | ptr[0];

	list_for_each(item, &diag_cmds) {
		dc = container_of(item, struct diag_cmd, node);
		if (key < dc->first || key > dc->last)
			continue;

		peripheral = dc->peripheral;

		if (peripheral->features & DIAG_FEATURE_APPS_HDLC_ENCODE)
			queue_push(&peripheral->dataq, ptr, len);
		else
			hdlc_enqueue(&dc->peripheral->dataq, ptr, len);

		handled++;
	}

	free(outbuf);

	return handled ? 0 : -ENOENT;
}

static void diag_rsp_bad_command(struct diag_client *client,
				 uint8_t *msg, size_t len)
{
	uint8_t *buf;

	buf = malloc(len + 1);
	if (!buf)
		err(1, "failed to allocate error buffer");

	buf[0] = 0x13;
	memcpy(buf + 1, msg, len);

	hdlc_enqueue(&client->outq, buf, len + 1);

	free(buf);
}

static int diag_sock_recv(int fd, void *data)
{
	struct diag_client *client = data;
	uint8_t buf[4096];
	uint8_t *ptr;
	uint8_t *msg;
	size_t msglen;
	size_t len;
	ssize_t n;
	int ret;

	n = read(fd, buf, sizeof(buf));
	if (n < 0)
		return n;

	ptr = buf;
	len = n;
	for (;;) {
		msg = hdlc_decode_one(&ptr, &len, &msglen);
		if (!msg)
			break;

		ret = diag_cmd_dispatch(msg, msglen);
		if (ret < 0)
			diag_rsp_bad_command(client, msg, msglen);
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct diag_client *qxdm;
	int ret;

	qxdm = malloc(sizeof(*qxdm));
	memset(qxdm, 0, sizeof(*qxdm));

	ret = diag_sock_connect("10.0.1.45", 2500);
	if (ret < 0)
		err(1, "failed to connect to qxdm");
	qxdm->fd = ret;
	qxdm->name = "QXDM";

	watch_add_readfd(ret, diag_sock_recv, qxdm);
	watch_add_writeq(qxdm->fd, &qxdm->outq);
	list_add(&diag_clients, &qxdm->node);

	peripheral_init();

	watch_run();

	return 0;
}
