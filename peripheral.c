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

#include "diag.h"
#include "diag_cntl.h"
#include "list.h"
#include "peripheral.h"
#include "util.h"
#include "watch.h"

struct devnode {
	char *devnode;
	char *name;
	char *rproc;

	struct list_head node;
};

struct list_head peripherals = LIST_INIT(peripherals);
struct list_head devnodes = LIST_INIT(devnodes);

static struct devnode *devnode_get(const char *devnode)
{
	struct list_head *item;
	struct devnode *node;

	list_for_each(item, &devnodes) {
		node = container_of(item, struct devnode, node);
		if (strcmp(node->devnode, devnode) == 0)
			return node;
	}

	return NULL;
}

static int devnode_open(const char *rproc, const char *name)
{
	struct list_head *item;
	struct devnode *node;

	list_for_each(item, &devnodes) {
		node = container_of(item, struct devnode, node);
		if (strcmp(node->rproc, rproc) == 0 &&
		    strcmp(node->name, name) == 0)
			return open(node->devnode, O_RDWR);
	}

	return -1;
}

static void devnode_add(const char *devnode, const char *name, const char *rproc)
{
	struct devnode *node;

	node = devnode_get(devnode);
	if (node) {
		warnx("node already in list");
		return;
	}

	node = malloc(sizeof(*node));
	memset(node, 0, sizeof(*node));

	node->devnode = strdup(devnode);
	node->name = strdup(name);
	node->rproc = strdup(rproc);

	list_add(&devnodes, &node->node);
}

static void devnode_remove(const char *devnode)
{
	struct devnode *node;

	node = devnode_get(devnode);
	if (!node)
		return;

	list_del(&node->node);

	free(node->name);
	free(node->devnode);
	free(node->rproc);
}

static const char *peripheral_udev_get_name(struct udev_device *dev)
{
	return udev_device_get_sysattr_value(dev, "name");
}

static const char *peripheral_udev_get_remoteproc(struct udev_device *dev)
{
	struct udev_device *parent;
	const char *p;

	parent = udev_device_get_parent(dev);
	if (!parent)
		return NULL;

	p = udev_device_get_sysattr_value(parent, "rpmsg_name");
	if (p)
		return p;

	return peripheral_udev_get_remoteproc(parent);
}

static void peripheral_open(void *data)
{
	struct peripheral *peripheral = data;
	char *rproc = peripheral->name;
	int ret;
	int fd;

	fd = devnode_open(rproc, "DIAG");
	if (fd < 0)
		fd = devnode_open(rproc, "APPS_RIVA_DATA");
	if (fd < 0) {
		warn("unable to open DIAG channel\n");
		return;
	}
	peripheral->data_fd = fd;

	fd = devnode_open(rproc, "DIAG_CNTL");
	if (fd < 0)
		fd = devnode_open(rproc, "APPS_RIVA_CTRL");
	if (fd < 0) {
		warn("unable to find DIAG_CNTL channel\n");
		close(peripheral->data_fd);
		peripheral->data_fd = -1;
		return;
	}
	peripheral->cntl_fd = fd;

	fd = devnode_open(rproc, "DIAG_CMD");
	if (fd >= 0)
		peripheral->cmd_fd = fd;

	ret = fcntl(peripheral->data_fd, F_SETFL, O_NONBLOCK);
	if (ret < 0)
		warn("failed to turn DIAG non blocking");

	watch_add_writeq(peripheral->cntl_fd, &peripheral->cntlq);
	watch_add_writeq(peripheral->data_fd, &peripheral->dataq);
	watch_add_readfd(peripheral->cntl_fd, diag_cntl_recv, peripheral);
	watch_add_readfd(peripheral->data_fd, diag_data_recv, peripheral);
}

static int peripheral_create(const char *name)
{
	struct peripheral *peripheral;
	struct list_head *item;

	list_for_each(item, &peripherals) {
		peripheral = container_of(item, struct peripheral, node);
		if (strcmp(peripheral->name, name) == 0)
			return 0;
	}

	peripheral = malloc(sizeof(*peripheral));
	memset(peripheral, 0, sizeof(*peripheral));

	peripheral->name = strdup(name);
	peripheral->data_fd = -1;
	peripheral->cntl_fd = -1;
	peripheral->cmd_fd = -1;
	list_add(&peripherals, &peripheral->node);

	watch_add_timer(peripheral_open, peripheral, 1000, false);

	return 0;
}

void peripheral_close(struct peripheral *peripheral)
{
	diag_cntl_close(peripheral);

	watch_remove_fd(peripheral->data_fd);
	watch_remove_fd(peripheral->cntl_fd);
	watch_remove_fd(peripheral->cmd_fd);

	close(peripheral->data_fd);
	close(peripheral->cntl_fd);
	close(peripheral->cmd_fd);

	list_del(&peripheral->node);
	free(peripheral->name);
	free(peripheral);
}

static int peripheral_udev_update(int fd, void *data)
{
	struct udev_monitor *mon = data;
	struct udev_device *dev;
	const char *devnode;
	const char *action;
	const char *rproc;
	const char *name;

	dev = udev_monitor_receive_device(mon);
	if (!dev)
		return 0;

	action = udev_device_get_action(dev);
	devnode = udev_device_get_devnode(dev);

	if (!devnode)
		goto unref_dev;

	if (strcmp(action, "add") == 0) {
		name = peripheral_udev_get_name(dev);
		rproc = peripheral_udev_get_remoteproc(dev);

		if (!name || !rproc)
			goto unref_dev;

		devnode_add(devnode, name, rproc);

		peripheral_create(rproc);
	} else if (strcmp(action, "remove") == 0) {
		devnode_remove(devnode);
	} else {
		warn("unknown udev action");
	}

unref_dev:
	udev_device_unref(dev);

	return 0;
}

int peripheral_init(void)
{
	struct udev_list_entry *devices;
	struct udev_list_entry *entry;
	struct udev_enumerate *enu;
	struct udev_monitor *mon;
	struct udev_device *dev;
	struct udev *udev;
	const char *devnode;
	const char *path;
	const char *rproc;
	const char *name;
	int fd;

	udev = udev_new();
	if (!udev)
		err(1, "failed to initialize libudev");

	mon = udev_monitor_new_from_netlink(udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(mon, "rpmsg", NULL);
	udev_monitor_enable_receiving(mon);

	fd = udev_monitor_get_fd(mon);

	enu = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enu, "rpmsg");
	udev_enumerate_scan_devices(enu);

	devices = udev_enumerate_get_list_entry(enu);
	udev_list_entry_foreach(entry, devices) {
		path = udev_list_entry_get_name(entry);
		dev = udev_device_new_from_syspath(udev, path);

		devnode = udev_device_get_devnode(dev);
		name = peripheral_udev_get_name(dev);
		rproc = peripheral_udev_get_remoteproc(dev);

		if (devnode && name && rproc) {
			devnode_add(devnode, name, rproc);
			peripheral_create(rproc);
		}

		udev_device_unref(dev);
	}

	watch_add_readfd(fd, peripheral_udev_update, mon);

	return 0;
}
