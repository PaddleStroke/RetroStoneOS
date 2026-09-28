/*
 * usb.c - USB mass storage hotplug without udev.
 *
 * Detection: the kernel uevent netlink socket only tells us "something in
 * the block subsystem changed"; the truth is always re-read from
 * /sys/block (a readdir of ~10 entries). A disk counts as USB when its
 * sysfs path goes through a USB host ("/usb" in the /sys/block/<disk> link
 * target), which excludes the SD card (mmcblk*) and the Pro's SATA disk.
 *
 * A new disk is left alone for settle_ms (partitions and the card in a USB
 * card reader show up a moment after the disk). Then each partition (or
 * the whole disk when it has no partition table) is probed by reading its
 * boot sector, and FAT/exFAT/NTFS ones are mounted read-only, nosuid,
 * nodev, noexec under <mount_base>/usbN.
 *
 * Read-only means pulling the stick out is always safe for the stick. A
 * pulled stick is lazily unmounted (MNT_DETACH) and reported as REMOVED.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "tr_internal.h"

#ifndef MS_NOSYMFOLLOW
#define MS_NOSYMFOLLOW 256     /* Linux 5.10, older libc headers */
#endif

#define MAX_DISKS 8
#define MAX_DRIVES 8
#define MAX_PARTS 16
#define QLEN 16

struct disk {
	bool used;
	char name[32];
	int64_t first_seen;
	uint64_t sig;          /* partition names + sizes when last handled */
	bool handled;
	bool ejected;          /* user ejected: do not remount until re-plugged */
};

struct drive {
	bool used;
	bool rw;                /* remounted read-write (backup) */
	char data[48];          /* the mount options that worked */
	struct transfer_usb_drive d;
};

static struct {
	bool init;
	char mount_base[58];     /* + "/usbNN" must fit transfer_usb_drive.mountpoint[64] */
	char sys_block[256];
	char dev_dir[64];
	int settle_ms, rescan_ms;
	transfer_mount_fn mount_fn;
	transfer_umount_fn umount_fn;
	int nlfd;
	bool inotify;            /* nlfd is an inotify fd on sys_block (tests) */
	char sys_disk[32];       /* the disk holding / (never a "USB drive"), "" = none */
	bool dirty;
	int64_t last_scan;
	struct disk disks[MAX_DISKS];
	struct drive drives[MAX_DRIVES];
	struct transfer_usb_event q[QLEN];
	int qhead, qlen;
} U = { .nlfd = -1 };

/* ------------------------------------------------------------ helpers */

/*
 * The disk that holds the root filesystem: /sys/dev/block/<maj>:<min> of
 * "/" links to .../block/<disk>/<partition> (or .../block/<disk>). Only
 * for the real /sys/block (tests use their own tree).
 */
static void find_sys_disk(char *out, size_t n)
{
	char p[64], link[512], *last, *prev;
	struct stat st;
	ssize_t len;

	out[0] = 0;
	if (strcmp(U.sys_block, "/sys/block") || stat("/", &st) < 0)
		return;
	snprintf(p, sizeof(p), "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
	len = readlink(p, link, sizeof(link) - 1);
	if (len <= 0)
		return;
	link[len] = 0;
	last = strrchr(link, '/');
	if (!last)
		return;
	*last++ = 0;
	prev = strrchr(link, '/');
	prev = prev ? prev + 1 : link;
	/* <disk>/<disk><partition>: the partition's parent is the disk */
	tr_strlcpy(out, strncmp(last, prev, strlen(prev)) || !strcmp(prev, "block") ? last : prev, n);
}

static void push(enum transfer_usb_event_type t, const struct transfer_usb_drive *d, int err)
{
	struct transfer_usb_event *e;

	if (U.qlen == QLEN) {                   /* drop the oldest */
		U.qhead = (U.qhead + 1) % QLEN;
		U.qlen--;
	}
	e = &U.q[(U.qhead + U.qlen) % QLEN];
	memset(e, 0, sizeof(*e));
	e->type = t;
	if (d)
		e->drive = *d;
	e->err = err;
	U.qlen++;
}

static int read_small(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -errno;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -errno;
	buf[r] = 0;
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
		buf[--r] = 0;
	return (int)r;
}

static uint64_t read_u64(const char *path)
{
	char b[32];

	if (read_small(path, b, sizeof(b)) <= 0)
		return 0;
	return strtoull(b, NULL, 10);
}

static bool is_usb_disk(const char *name)
{
	char p[512], link[1024];
	ssize_t n;

	if (!strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) || !strncmp(name, "mmcblk", 6) ||
	    !strncmp(name, "zram", 4))
		return false;
	/* The system disk (a board booting from USB: a Raspberry Pi on an SSD) */
	if (U.sys_disk[0] && !strcmp(name, U.sys_disk))
		return false;
	snprintf(p, sizeof(p), "%s/%s", U.sys_block, name);
	n = readlink(p, link, sizeof(link) - 1);
	if (n <= 0)
		return false;
	link[n] = 0;
	return strstr(link, "/usb") != NULL;
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
	const unsigned char *p = data;

	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

struct part {
	char name[32];
	uint64_t size;
};

/* Partitions of a disk ("sda1"...), or the disk itself if it has none. */
static int list_parts(const char *disk, struct part *out, int max, uint64_t *sig)
{
	char p[320];
	DIR *d;
	struct dirent *de;
	int n = 0;

	*sig = 14695981039346656037ull;
	snprintf(p, sizeof(p), "%s/%s", U.sys_block, disk);
	d = opendir(p);
	if (!d)
		return 0;
	while ((de = readdir(d)) && n < max) {
		char pp[640];
		struct stat st;

		if (strncmp(de->d_name, disk, strlen(disk)) || !de->d_name[strlen(disk)])
			continue;
		snprintf(pp, sizeof(pp), "%s/%s/partition", p, de->d_name);
		if (stat(pp, &st) < 0)
			continue;
		tr_strlcpy(out[n].name, de->d_name, sizeof(out[n].name));
		snprintf(pp, sizeof(pp), "%s/%s/size", p, de->d_name);
		out[n].size = read_u64(pp) * 512;
		n++;
	}
	closedir(d);
	if (n == 0) {
		snprintf(p, sizeof(p), "%s/%s/size", U.sys_block, disk);
		tr_strlcpy(out[0].name, disk, sizeof(out[0].name));
		out[0].size = read_u64(p) * 512;
		n = 1;
	}
	/* stable order: sda1, sda2... (readdir order is arbitrary) */
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && strcmp(out[j - 1].name, out[j].name) > 0; j--) {
			struct part t = out[j];

			out[j] = out[j - 1];
			out[j - 1] = t;
		}
	for (int i = 0; i < n; i++) {
		*sig = fnv(*sig, out[i].name, strlen(out[i].name));
		*sig = fnv(*sig, &out[i].size, sizeof(out[i].size));
	}
	return n;
}

/* ------------------------------------------------------------ fs probe */

static void utf16le_to_utf8(const unsigned char *in, int units, char *out, size_t n)
{
	size_t o = 0;

	for (int i = 0; i < units; i++) {
		unsigned c = in[2 * i] | (in[2 * i + 1] << 8);

		if (!c)
			break;
		if (c >= 0xd800 && c <= 0xdfff)
			c = '?';                          /* labels: BMP is enough */
		if (c < 0x80 && o + 1 < n) {
			out[o++] = (char)c;
		} else if (c < 0x800 && o + 2 < n) {
			out[o++] = (char)(0xc0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3f));
		} else if (o + 3 < n) {
			out[o++] = (char)(0xe0 | (c >> 12));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
			out[o++] = (char)(0x80 | (c & 0x3f));
		}
	}
	out[o] = 0;
}

static void trim_label(const unsigned char *src, size_t n, char *out, size_t on)
{
	size_t l = n < on - 1 ? n : on - 1, o = 0;

	for (size_t i = 0; i < l; i++)
		out[o++] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : '_';
	while (o > 0 && out[o - 1] == ' ')
		o--;
	out[o] = 0;
	if (!strcmp(out, "NO NAME"))
		out[0] = 0;
}

static void exfat_label(int fd, const unsigned char *bs, char *label, size_t ln)
{
	unsigned bps_shift = bs[108], spc_shift = bs[109];
	uint32_t heap = bs[88] | bs[89] << 8 | bs[90] << 16 | (uint32_t)bs[91] << 24;
	uint32_t root = bs[96] | bs[97] << 8 | bs[98] << 16 | (uint32_t)bs[99] << 24;
	size_t clus;
	unsigned char *buf;
	off_t off;

	if (bps_shift < 9 || bps_shift > 12 || spc_shift > 25 - bps_shift || root < 2)
		return;
	clus = (size_t)1 << (bps_shift + spc_shift);
	if (clus > 64 * 1024)
		clus = 64 * 1024;               /* the label is in the first entries */
	off = ((off_t)heap + ((off_t)(root - 2) << spc_shift)) << bps_shift;
	buf = malloc(clus);
	if (!buf)
		return;
	if (pread(fd, buf, clus, off) == (ssize_t)clus) {
		for (size_t i = 0; i + 32 <= clus; i += 32) {
			if (buf[i] == 0x00)
				break;
			if (buf[i] == 0x83) {           /* volume label entry */
				int cnt = buf[i + 1] > 11 ? 11 : buf[i + 1];

				utf16le_to_utf8(buf + i + 2, cnt, label, ln);
				break;
			}
		}
	}
	free(buf);
}

const char *transfer_fs_probe(const char *path, char *label, size_t label_n)
{
	unsigned char bs[512];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	const char *type = NULL;
	char lbl[64] = "";

	if (fd < 0)
		return NULL;
	if (pread(fd, bs, sizeof(bs), 0) != (ssize_t)sizeof(bs))
		goto out;
	if (!memcmp(bs + 3, "EXFAT   ", 8)) {
		type = "exfat";
		exfat_label(fd, bs, lbl, sizeof(lbl));
	} else if (!memcmp(bs + 3, "NTFS    ", 8)) {
		type = "ntfs";
	} else if (bs[510] == 0x55 && bs[511] == 0xaa) {
		unsigned bps = bs[11] | bs[12] << 8, spc = bs[13];
		unsigned rsvd = bs[14] | bs[15] << 8, nfat = bs[16];
		bool bpb_ok = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) &&
			      spc && !(spc & (spc - 1)) && rsvd >= 1 && (nfat == 1 || nfat == 2);

		if (bpb_ok && !memcmp(bs + 82, "FAT32", 5)) {
			type = "vfat";
			trim_label(bs + 71, 11, lbl, sizeof(lbl));
		} else if (bpb_ok && (!memcmp(bs + 54, "FAT1", 4) || !memcmp(bs + 54, "FAT ", 4))) {
			type = "vfat";
			trim_label(bs + 43, 11, lbl, sizeof(lbl));
		} else if (bpb_ok && (bs[0] == 0xeb || bs[0] == 0xe9)) {
			type = "vfat";                  /* old DOS formatters, no type string */
		}
	}
out:
	close(fd);
	if (label && label_n)
		tr_strlcpy(label, type ? lbl : "", label_n);
	return type;
}

/* ------------------------------------------------------------ mount */

static int do_mount(const char *src, const char *dst, const char *fs, const char *data)
{
	/* MS_NOSYMFOLLOW (Linux 5.10): symlinks on the stick (NTFS reparse
	 * points, "bios -> ../../..") are never followed out of it (F-M16) */
	unsigned long fl = MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_NOSYMFOLLOW;
	int r = U.mount_fn ? U.mount_fn(src, dst, fs, fl, data) : mount(src, dst, fs, fl, data);

	return r < 0 ? -errno : 0;
}

static int do_umount(const char *target, int flags)
{
	int r = U.umount_fn ? U.umount_fn(target, flags) : umount2(target, flags);

	return r < 0 ? -errno : 0;
}

static int mkdir_p(const char *path)
{
	char tmp[512];

	if (tr_strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp))
		return -ENAMETOOLONG;
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -errno;
		*p = '/';
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		return -errno;
	return 0;
}

/* Tries the mount variants for a probed type; sets fstype and data to what
 * worked. */
static int mount_fs(const char *dev, const char *mnt, const char *probed, char *fstype, size_t fn,
		    char *data, size_t dn)
{
	int r = -ENODEV;
	const char *opt = NULL;

	if (!strcmp(probed, "vfat")) {
		/* utf8=1: UTF-8 names without the "iocharset=utf8" case warning */
		r = do_mount(dev, mnt, "vfat", opt = "utf8=1,shortname=mixed");
		if (r == -EINVAL)
			r = do_mount(dev, mnt, "vfat", opt = NULL);
		tr_strlcpy(fstype, "vfat", fn);
	} else if (!strcmp(probed, "exfat")) {
		r = do_mount(dev, mnt, "exfat", opt = "iocharset=utf8");
		if (r == -EINVAL)
			r = do_mount(dev, mnt, "exfat", opt = NULL);
		tr_strlcpy(fstype, "exfat", fn);
	} else if (!strcmp(probed, "ntfs")) {
		/* ntfs3 (module, auto-loaded by the kernel through modprobe on the
		 * first mount); "ntfs" is its legacy alias on 6.9+ kernels. */
		r = do_mount(dev, mnt, "ntfs3", opt = "iocharset=utf8");
		tr_strlcpy(fstype, "ntfs3", fn);
		if (r == -ENODEV) {
			r = do_mount(dev, mnt, "ntfs", opt = NULL);
			tr_strlcpy(fstype, "ntfs", fn);
		}
	}
	tr_strlcpy(data, opt ? opt : "", dn);
	return r;
}

static int free_index(void)
{
	for (int idx = 0; idx < MAX_DRIVES; idx++) {
		bool taken = false;

		for (int i = 0; i < MAX_DRIVES; i++)
			if (U.drives[i].used && U.drives[i].d.index == idx)
				taken = true;
		if (!taken)
			return idx;
	}
	return -1;
}

static struct drive *drive_slot(void)
{
	for (int i = 0; i < MAX_DRIVES; i++)
		if (!U.drives[i].used)
			return &U.drives[i];
	return NULL;
}

static void read_vendor(const char *disk, char *out, size_t n)
{
	char p[512], v[64] = "", m[64] = "";

	snprintf(p, sizeof(p), "%s/%s/device/vendor", U.sys_block, disk);
	read_small(p, v, sizeof(v));
	snprintf(p, sizeof(p), "%s/%s/device/model", U.sys_block, disk);
	read_small(p, m, sizeof(m));
	/* vendor + model, cut to the caller's buffer (display only) */
	snprintf(out, n, "%.23s%s%.23s", v, v[0] && m[0] ? " " : "", m);
}

static bool part_mounted(const char *dev)
{
	for (int i = 0; i < MAX_DRIVES; i++)
		if (U.drives[i].used && !strcmp(U.drives[i].d.dev, dev))
			return true;
	return false;
}

static void handle_disk(struct disk *dk, const struct part *parts, int np)
{
	bool any_media = false, any_supported = false;
	struct transfer_usb_drive info;

	memset(&info, 0, sizeof(info));
	tr_strlcpy(info.disk, dk->name, sizeof(info.disk));
	read_vendor(dk->name, info.vendor, sizeof(info.vendor));
	{
		char sz[32], disk_sz[512];

		tr_snprintf(disk_sz, sizeof(disk_sz), "%s/%s/size", U.sys_block, dk->name);
		tr_fmt_bytes(read_u64(disk_sz) * 512, sz, sizeof(sz));
		tr_log("usb: disk %s: \"%s\", %s, %d partition%s%s", dk->name, info.vendor, sz, np,
		       np == 1 ? "" : "s", np == 1 && !strcmp(parts[0].name, dk->name) ? " (no table)" : "");
	}

	for (int i = 0; i < np; i++) {
		char dev[128], label[64], fstype[16], data[48], sz[32];
		const char *probed;
		struct drive *slot;
		int idx, r;

		if (parts[i].size == 0)
			continue;
		any_media = true;
		if (part_mounted(parts[i].name)) {
			any_supported = true;
			continue;
		}
		if (tr_snprintf(dev, sizeof(dev), "%s/%s", U.dev_dir, parts[i].name) < 0)
			continue;
		tr_fmt_bytes(parts[i].size, sz, sizeof(sz));
		probed = transfer_fs_probe(dev, label, sizeof(label));
		if (!probed) {
			tr_log("usb: %s (%s): no FAT, exFAT or NTFS filesystem, not mounted", parts[i].name, sz);
			continue;
		}
		any_supported = true;
		idx = free_index();
		slot = drive_slot();
		if (idx < 0 || !slot) {
			tr_log("usb: %s: too many drives, not mounted", parts[i].name);
			break;
		}
		tr_strlcpy(info.dev, parts[i].name, sizeof(info.dev));
		tr_strlcpy(info.label, label, sizeof(info.label));
		info.size_bytes = parts[i].size;
		info.index = idx;
		snprintf(info.mountpoint, sizeof(info.mountpoint), "%s/usb%d", U.mount_base, idx);
		if ((r = mkdir_p(info.mountpoint)) == 0)
			r = mount_fs(dev, info.mountpoint, probed, fstype, sizeof(fstype), data, sizeof(data));
		if (r < 0) {
			tr_strlcpy(info.fstype, probed, sizeof(info.fstype));
			tr_log("usb: %s (%s, \"%s\", %s): mount on %s failed: %s", parts[i].name, probed,
			       label, sz, info.mountpoint, strerror(-r));
			rmdir(info.mountpoint);
			push(TRANSFER_USB_MOUNT_FAILED, &info, r);
			continue;
		}
		tr_strlcpy(info.fstype, fstype, sizeof(info.fstype));
		slot->used = true;
		slot->rw = false;
		tr_strlcpy(slot->data, data, sizeof(slot->data));
		slot->d = info;
		tr_log("usb: %s (%s, \"%s\", %s) mounted read-only on %s", parts[i].name, fstype, label, sz,
		       info.mountpoint);
		push(TRANSFER_USB_MOUNTED, &info, 0);
	}
	if (any_media && !any_supported) {
		tr_log("usb: disk %s: no supported partition (use FAT32 or exFAT)", dk->name);
		push(TRANSFER_USB_UNSUPPORTED, &info, 0);
	} else if (!any_media) {
		tr_log("usb: disk %s: no media (card reader?), waiting for a card", dk->name);
	}
	/* No media (empty card reader): stay unhandled, a "change" uevent comes
	 * with the card. */
	dk->handled = any_media;
}

static void drop_drive(struct drive *dr, enum transfer_usb_event_type why)
{
	int r = do_umount(dr->d.mountpoint, MNT_DETACH);

	if (r < 0 && r != -EINVAL && r != -ENOENT)
		tr_log("umount %s: %s", dr->d.mountpoint, strerror(-r));
	tr_log("usb: %s removed (was on %s%s)", dr->d.dev, dr->d.mountpoint,
	       dr->rw ? ", read-write: pulled while writing?" : "");
	rmdir(dr->d.mountpoint);
	push(why, &dr->d, 0);
	dr->used = false;
}

static void scan(void)
{
	DIR *d = opendir(U.sys_block);
	struct dirent *de;
	bool seen[MAX_DISKS] = { false };
	int64_t now = tr_now_ms();

	U.last_scan = now;
	U.dirty = false;
	if (!d)
		return;
	while ((de = readdir(d))) {
		struct disk *dk = NULL, *freeslot = NULL;

		if (de->d_name[0] == '.' || !is_usb_disk(de->d_name))
			continue;
		for (int i = 0; i < MAX_DISKS; i++) {
			if (U.disks[i].used && !strcmp(U.disks[i].name, de->d_name))
				dk = &U.disks[i];
			else if (!U.disks[i].used && !freeslot)
				freeslot = &U.disks[i];
		}
		if (!dk) {
			if (!freeslot)
				continue;
			dk = freeslot;
			memset(dk, 0, sizeof(*dk));
			dk->used = true;
			tr_strlcpy(dk->name, de->d_name, sizeof(dk->name));
			dk->first_seen = now;
		}
		seen[dk - U.disks] = true;
	}
	closedir(d);

	for (int i = 0; i < MAX_DISKS; i++) {
		struct disk *dk = &U.disks[i];
		struct part parts[MAX_PARTS];
		uint64_t sig;
		int np;

		if (!dk->used)
			continue;
		if (!seen[i]) {                        /* unplugged */
			for (int j = 0; j < MAX_DRIVES; j++)
				if (U.drives[j].used && !strcmp(U.drives[j].d.disk, dk->name))
					drop_drive(&U.drives[j], TRANSFER_USB_REMOVED);
			dk->used = false;
			continue;
		}
		if (now - dk->first_seen < U.settle_ms)
			continue;
		np = list_parts(dk->name, parts, MAX_PARTS, &sig);
		/* partitions of mounted drives that vanished (card pulled from a
		 * reader, table rewritten) */
		for (int j = 0; j < MAX_DRIVES; j++) {
			bool still = false;

			if (!U.drives[j].used || strcmp(U.drives[j].d.disk, dk->name))
				continue;
			for (int k = 0; k < np; k++)
				if (!strcmp(parts[k].name, U.drives[j].d.dev) && parts[k].size)
					still = true;
			if (!still)
				drop_drive(&U.drives[j], TRANSFER_USB_REMOVED);
		}
		if (dk->handled && sig == dk->sig)
			continue;
		if (dk->ejected && sig == dk->sig)
			continue;
		dk->sig = sig;
		dk->ejected = false;
		handle_disk(dk, parts, np);
	}
}

/* A disk seen recently that is not handled yet: rescan until it settles.
 * An empty card reader stays unhandled; its card arrives with a uevent. */
static bool settle_pending(int64_t now)
{
	for (int i = 0; i < MAX_DISKS; i++)
		if (U.disks[i].used && !U.disks[i].handled && !U.disks[i].ejected &&
		    now - U.disks[i].first_seen < U.settle_ms + 3000)
			return true;
	return false;
}

/* ------------------------------------------------------------ netlink */

static int nl_open(void)
{
	struct sockaddr_nl a;
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
			NETLINK_KOBJECT_UEVENT);

	if (fd < 0)
		return -errno;
	memset(&a, 0, sizeof(a));
	a.nl_family = AF_NETLINK;
	a.nl_groups = 1;                           /* kernel uevents */
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		int e = -errno;

		close(fd);
		return e;
	}
	return fd;
}

static void nl_drain(void)
{
	char buf[4096];

	if (U.inotify) {                           /* tests: any change of sys_block */
		ssize_t n;

		while ((n = read(U.nlfd, buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR))
			if (n > 0)
				U.dirty = true;
		return;
	}

	for (;;) {
		struct sockaddr_nl src;
		struct iovec iov = { buf, sizeof(buf) - 1 };
		struct msghdr mh = { .msg_name = &src, .msg_namelen = sizeof(src),
				     .msg_iov = &iov, .msg_iovlen = 1 };
		ssize_t n = recvmsg(U.nlfd, &mh, MSG_DONTWAIT);

		if (n < 0) {
			if (errno == ENOBUFS)          /* overflow: state unknown */
				U.dirty = true;
			if (errno == EINTR || errno == ENOBUFS)
				continue;
			return;
		}
		if (src.nl_pid != 0)               /* only the kernel */
			continue;
		buf[n] = 0;
		for (ssize_t off = 0; off < n; off += (ssize_t)strlen(buf + off) + 1)
			if (!strcmp(buf + off, "SUBSYSTEM=block")) {
				U.dirty = true;
				break;
			}
	}
}

/* ------------------------------------------------------------ API */

int transfer_usb_init(const struct transfer_usb_config *cfg)
{
	struct transfer_usb_config def = { 0 };

	if (U.init)
		return 0;
	if (!cfg)
		cfg = &def;
	if ((cfg->mount_base && strlen(cfg->mount_base) >= sizeof(U.mount_base)) ||
	    (cfg->dev_dir && strlen(cfg->dev_dir) >= sizeof(U.dev_dir)) ||
	    (cfg->sys_block && strlen(cfg->sys_block) >= sizeof(U.sys_block)))
		return -ENAMETOOLONG;
	memset(&U, 0, sizeof(U));
	U.nlfd = -1;
	tr_strlcpy(U.mount_base, cfg->mount_base ? cfg->mount_base : "/media", sizeof(U.mount_base));
	tr_strlcpy(U.sys_block, cfg->sys_block ? cfg->sys_block : "/sys/block", sizeof(U.sys_block));
	tr_strlcpy(U.dev_dir, cfg->dev_dir ? cfg->dev_dir : "/dev", sizeof(U.dev_dir));
	find_sys_disk(U.sys_disk, sizeof(U.sys_disk));
	U.settle_ms = cfg->settle_ms > 0 ? cfg->settle_ms : 700;
	U.rescan_ms = cfg->rescan_ms > 0 ? cfg->rescan_ms : 1000;
	{
		/* /media is a symlink to /run/media on the read-only root; /run is
		 * a fresh tmpfs at every boot, so create the target. */
		char t[256];
		ssize_t n = readlink(U.mount_base, t, sizeof(t) - 1);

		if (n > 0 && t[0] == '/') {
			t[n] = 0;
			mkdir_p(t);
		}
	}
	U.mount_fn = cfg->mount_fn;
	U.umount_fn = cfg->umount_fn;
	if (cfg->watch_sys_block) {
		U.nlfd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (U.nlfd >= 0 && inotify_add_watch(U.nlfd, U.sys_block, IN_CREATE | IN_DELETE |
						     IN_MOVED_FROM | IN_MOVED_TO) < 0) {
			close(U.nlfd);
			U.nlfd = -1;
		}
		U.inotify = U.nlfd >= 0;
	} else if (!cfg->no_netlink) {
		U.nlfd = nl_open();
		if (U.nlfd < 0)
			tr_log("uevent socket: %s, polling /sys/block instead", strerror(-U.nlfd));
	}
	U.init = true;
	U.dirty = true;
	tr_log("usb: watching %s (%s)", U.sys_block, U.inotify ? "inotify, test mode" :
	       U.nlfd >= 0 ? "kernel uevents" : "polled every second");
	return 0;
}

int transfer_usb_fd(void)
{
	return U.init ? U.nlfd : -1;
}

int transfer_usb_timeout_ms(void)
{
	int64_t now;

	if (!U.init)
		return -1;
	if (U.dirty || U.qlen)
		return 0;
	now = tr_now_ms();
	if (U.nlfd < 0) {
		int64_t d = U.last_scan + U.rescan_ms - now;

		return d < 0 ? 0 : (int)d;
	}
	if (settle_pending(now)) {
		int64_t d = U.last_scan + 200 - now;

		return d < 0 ? 0 : (int)d;
	}
	return -1;
}

int transfer_usb_poll(struct transfer_usb_event *ev)
{
	if (!U.init)
		return 0;
	if (U.nlfd >= 0)
		nl_drain();
	if (!U.qlen) {
		int64_t now = tr_now_ms();
		bool periodic;

		if (U.nlfd < 0)
			periodic = now - U.last_scan >= U.rescan_ms;
		else
			periodic = settle_pending(now) && now - U.last_scan >= 200;
		if (U.dirty || periodic)
			scan();
	}
	if (!U.qlen)
		return 0;
	*ev = U.q[U.qhead];
	U.qhead = (U.qhead + 1) % QLEN;
	U.qlen--;
	return 1;
}

int transfer_usb_drives(struct transfer_usb_drive *out, int max)
{
	int n = 0;

	for (int i = 0; i < MAX_DRIVES && n < max; i++)
		if (U.drives[i].used)
			out[n++] = U.drives[i].d;
	return n;
}

int transfer_usb_eject(const char *mountpoint)
{
	for (int i = 0; i < MAX_DRIVES; i++) {
		struct drive *dr = &U.drives[i];
		int r;

		if (!dr->used || strcmp(dr->d.mountpoint, mountpoint))
			continue;
		r = do_umount(dr->d.mountpoint, 0);
		tr_log("usb: eject %s (%s): %s", dr->d.mountpoint, dr->d.dev, r < 0 ? strerror(-r) : "done");
		if (r < 0)
			return r;
		for (int k = 0; k < MAX_DISKS; k++)
			if (U.disks[k].used && !strcmp(U.disks[k].name, dr->d.disk))
				U.disks[k].ejected = true;
		rmdir(dr->d.mountpoint);
		push(TRANSFER_USB_EJECTED, &dr->d, 0);
		dr->used = false;
		return 0;
	}
	return -ENOENT;
}

int transfer_usb_remount(const char *mountpoint, bool writable)
{
	for (int i = 0; i < MAX_DRIVES; i++) {
		struct drive *dr = &U.drives[i];
		unsigned long fl = MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_NOSYMFOLLOW;
		int r;

		if (!dr->used || strcmp(dr->d.mountpoint, mountpoint))
			continue;
		if (dr->rw == writable)
			return 0;
		if (!writable) {
			sync();                   /* then the remount has nothing to flush */
			fl |= MS_RDONLY;
		}
		r = U.mount_fn ? U.mount_fn(NULL, mountpoint, NULL, fl, dr->data[0] ? dr->data : NULL) :
				 mount(NULL, mountpoint, NULL, fl, dr->data[0] ? dr->data : NULL);
		r = r < 0 ? -errno : 0;
		tr_log("usb: %s remounted %s: %s", mountpoint, writable ? "read-write" : "read-only",
		       r < 0 ? strerror(-r) : "ok");
		if (r == 0)
			dr->rw = writable;
		return r;
	}
	return -ENOENT;
}

void transfer_usb_shutdown(void)
{
	if (!U.init)
		return;
	for (int i = 0; i < MAX_DRIVES; i++)
		if (U.drives[i].used) {
			do_umount(U.drives[i].d.mountpoint, MNT_DETACH);
			rmdir(U.drives[i].d.mountpoint);
			U.drives[i].used = false;
		}
	if (U.nlfd >= 0)
		close(U.nlfd);
	U.nlfd = -1;
	U.init = false;
}
