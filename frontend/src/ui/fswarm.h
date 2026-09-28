/*
 * fswarm.h - warm the kernel's cache of exFAT directories with a few large
 * reads.
 *
 * Why: the Linux exFAT driver reads a directory it has not cached yet with
 * one 512-byte request per sector of its first cluster (exfat_dir_readahead()
 * issues a breadahead per sector, which the SD host runs one by one), and a
 * plain stat() of a directory triggers it (exfat_find() counts the
 * subdirectories). On a card formatted with 32 KiB clusters that is 64
 * requests, ~110 ms on the RetroStone2's SD host (~1.6 ms per request), for
 * EVERY directory touched once per boot: 34 ROM folders cost ~3.9 s.
 *
 * fswarm walks the exFAT directory structure itself (boot sector, directory
 * entry sets) through the block device of the mount, and reads the clusters
 * of the directories the caller is about to use with one large pread() per
 * run of clusters. Those reads land in the block device's page cache, which is
 * the buffer cache the driver reads its directories from (same mapping), so the
 * driver's own lookups then find every sector cached and issue no request.
 * Read-only; on any other filesystem, or on any error, it does nothing and
 * the caller simply pays the normal cost. Thread-safe (one internal lock).
 *
 * The real fix is a two-line kernel change (a block plug around the
 * readahead loop, so the 64 requests merge into one); this module stays
 * harmless once that is in.
 */
#ifndef RSOS_UI_FSWARM_H
#define RSOS_UI_FSWARM_H

#include <stdbool.h>
#include <stdint.h>

#define FSW_CHILDREN 1u     /* also every subdirectory of each path (whole directories) */

struct fswarm_stats {
	int dirs;               /* directories warmed */
	int reads;              /* pread() calls */
	int64_t bytes;
	int64_t us;
};

/*
 * Warms every directory from the mount root down to each path (which must
 * be directories; missing ones are skipped), plus their subdirectories with
 * FSW_CHILDREN. Returns the number of directories warmed, 0 when the paths
 * are not on exFAT, < 0 on error. st may be NULL; it is added to.
 */
int fswarm(const char *const *paths, int n, unsigned flags, struct fswarm_stats *st);

/* Closes the cached block device handles (the next call re-opens them). */
void fswarm_close(void);

#endif
