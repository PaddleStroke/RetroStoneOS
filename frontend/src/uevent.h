/*
 * uevent.h - raw kernel uevent listener (NETLINK_KOBJECT_UEVENT).
 *
 * RetroStoneOS runs without udevd, so hotplug notifications are read
 * straight from the kernel's uevent multicast group 1. No libudev.
 */
#ifndef RSOS_UEVENT_H
#define RSOS_UEVENT_H

#include <stdbool.h>
#include <stddef.h>

/* Largest uevent the kernel sends is UEVENT_BUFFER_SIZE (2048) bytes. */
#define UEVENT_MSG_MAX 4096

struct uevent {
	/* All pointers point into buf and stay valid until the next read. */
	const char *action;    /* ACTION=    (add, remove, change, ...) */
	const char *devpath;   /* DEVPATH=   */
	const char *subsystem; /* SUBSYSTEM= */
	const char *devname;   /* DEVNAME=   (e.g. dri/card0) */
	const char *devtype;   /* DEVTYPE=   */
	bool hotplug;          /* HOTPLUG=1  (DRM connector hotplug) */
	int connector_id;      /* CONNECTOR= (per-connector hotplug), else 0 */
	int property_id;       /* PROPERTY=  (per-connector hotplug), else 0 */
	unsigned long long seqnum;

	size_t len;
	char buf[UEVENT_MSG_MAX + 1];
};

/* Opens a non-blocking, close-on-exec uevent socket. Returns fd or -errno. */
int uevent_open(void);

/*
 * Reads one message. Returns 1 when a kernel message was parsed into ev,
 * 0 when there is nothing (more) to read, -errno on error. Messages that
 * were not sent by the kernel (e.g. udevd's "libudev" monitor packets or
 * spoofed userspace senders) are skipped silently.
 */
int uevent_read(int fd, struct uevent *ev);

/* Looks up an arbitrary KEY= in a parsed message, NULL if absent. */
const char *uevent_get(const struct uevent *ev, const char *key);

/* True for a DRM "change" uevent carrying HOTPLUG=1. */
bool uevent_is_drm_hotplug(const struct uevent *ev);

void uevent_close(int fd);

#endif
