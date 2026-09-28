/*
 * uevent.c - raw kernel uevent listener (NETLINK_KOBJECT_UEVENT).
 *
 * Kernel message format (lib/kobject_uevent.c):
 *   "<action>@<devpath>\0KEY=VALUE\0KEY=VALUE\0..."
 * A DRM connector hotplug (drm_sysfs_hotplug_event() and
 * drm_sysfs_connector_hotplug_event()) looks like:
 *   change@/devices/platform/display-engine/drm/card0\0ACTION=change\0
 *   DEVPATH=...\0SUBSYSTEM=drm\0HOTPLUG=1\0[CONNECTOR=42\0PROPERTY=7\0]
 *   DEVNAME=dri/card0\0DEVTYPE=drm_minor\0SEQNUM=1234\0
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "uevent.h"

#include <errno.h>
#include <linux/netlink.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* Kernel uevents go to multicast group 1 (udevd re-broadcasts on group 2). */
#define UEVENT_GROUP_KERNEL 1u

int uevent_open(void)
{
	struct sockaddr_nl addr;
	int fd, sz = 1 << 20;

	fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
		    NETLINK_KOBJECT_UEVENT);
	if (fd < 0)
		return -errno;

	/*
	 * A burst of uevents at boot (every device "add") can overflow the
	 * default buffer. SO_RCVBUFFORCE needs CAP_NET_ADMIN (we normally run
	 * as root); fall back to the capped SO_RCVBUF otherwise. Overflow is
	 * not fatal anyway: uevent_read() reports -ENOBUFS and the caller
	 * re-probes the hardware state.
	 */
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz)) < 0)
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = 0; /* let the kernel pick a unique port id */
	addr.nl_groups = UEVENT_GROUP_KERNEL;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int err = -errno;
		close(fd);
		return err;
	}
	return fd;
}

void uevent_close(int fd)
{
	if (fd >= 0)
		close(fd);
}

static const char *match_key(const char *kv, const char *key)
{
	size_t n = strlen(key);

	if (strncmp(kv, key, n) == 0 && kv[n] == '=')
		return kv + n + 1;
	return NULL;
}

const char *uevent_get(const struct uevent *ev, const char *key)
{
	size_t off = strnlen(ev->buf, ev->len) + 1; /* skip "action@devpath" */

	while (off < ev->len) {
		const char *kv = ev->buf + off;
		const char *v = match_key(kv, key);

		if (v)
			return v;
		off += strnlen(kv, ev->len - off) + 1;
	}
	return NULL;
}

static void parse(struct uevent *ev)
{
	size_t off;

	ev->action = ev->devpath = ev->subsystem = NULL;
	ev->devname = ev->devtype = NULL;
	ev->hotplug = false;
	ev->connector_id = ev->property_id = 0;
	ev->seqnum = 0;

	off = strnlen(ev->buf, ev->len) + 1;
	while (off < ev->len) {
		const char *kv = ev->buf + off;
		const char *v;

		if ((v = match_key(kv, "ACTION")))
			ev->action = v;
		else if ((v = match_key(kv, "DEVPATH")))
			ev->devpath = v;
		else if ((v = match_key(kv, "SUBSYSTEM")))
			ev->subsystem = v;
		else if ((v = match_key(kv, "DEVNAME")))
			ev->devname = v;
		else if ((v = match_key(kv, "DEVTYPE")))
			ev->devtype = v;
		else if ((v = match_key(kv, "HOTPLUG")))
			ev->hotplug = (strcmp(v, "1") == 0);
		else if ((v = match_key(kv, "CONNECTOR")))
			ev->connector_id = atoi(v);
		else if ((v = match_key(kv, "PROPERTY")))
			ev->property_id = atoi(v);
		else if ((v = match_key(kv, "SEQNUM")))
			ev->seqnum = strtoull(v, NULL, 10);
		off += strnlen(kv, ev->len - off) + 1;
	}
}

int uevent_read(int fd, struct uevent *ev)
{
	for (;;) {
		struct sockaddr_nl src;
		struct iovec iov = { ev->buf, UEVENT_MSG_MAX };
		struct msghdr msg;
		ssize_t n;

		memset(&msg, 0, sizeof(msg));
		msg.msg_name = &src;
		msg.msg_namelen = sizeof(src);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		n = recvmsg(fd, &msg, MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			if (errno == EINTR)
				continue;
			return -errno; /* -ENOBUFS: overflow, caller re-probes */
		}
		if (n == 0)
			return 0;
		if (msg.msg_flags & MSG_TRUNC)
			continue;

		/* Only trust the kernel (port id 0, kernel multicast group). */
		if (msg.msg_namelen != sizeof(src) || src.nl_pid != 0)
			continue;

		ev->len = (size_t)n;
		ev->buf[n] = '\0';

		/* Kernel messages start with "action@devpath". */
		if (!memchr(ev->buf, '@', strnlen(ev->buf, ev->len)))
			continue;

		parse(ev);
		return 1;
	}
}

bool uevent_is_drm_hotplug(const struct uevent *ev)
{
	return ev->subsystem && strcmp(ev->subsystem, "drm") == 0 &&
	       ev->action && strcmp(ev->action, "change") == 0 &&
	       ev->hotplug;
}
