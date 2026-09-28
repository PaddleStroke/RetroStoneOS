/*
 * netnames.c - a minimal name responder, so that the console can be
 * reached by name on the LAN while networking is on:
 *
 *   mDNS  (RFC 6762, UDP 5353, 224.0.0.251): "retrostone.local" -> A
 *         (phones, macOS, Linux with nss-mdns, Windows 10 1803+)
 *   LLMNR (RFC 4795, UDP 5355, 224.0.0.252): "retrostone" -> A
 *         (Windows single-label names: \\RETROSTONE, http://retrostone/)
 *   NBNS  (RFC 1002, UDP 137, broadcast): "RETROSTONE<00>/<20>" -> A
 *         (older Windows, and Windows with LLMNR disabled)
 *
 * Only A records for our own name: no service discovery (DNS-SD), no
 * probing/conflict resolution, no IPv6. That is ~0 CPU and one thread that
 * sleeps in poll(). Avahi would add dbus + libdaemon + ~1 MB for features we
 * do not use (see docs/rom-transfer.md).
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tr_internal.h"

#define MDNS_GROUP "224.0.0.251"
#define LLMNR_GROUP "224.0.0.252"

/* ------------------------------------------------------------ packets */

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* Reads a (possibly compressed) DNS name as lowercase "a.b.c". Returns the
 * offset just after the name in the packet, or -1. */
static int read_name(const uint8_t *pkt, size_t len, size_t off, char *out, size_t outn)
{
	size_t o = 0, pos = off;
	int end = -1, jumps = 0;

	for (;;) {
		uint8_t l;

		if (pos >= len)
			return -1;
		l = pkt[pos];
		if (l == 0) {
			if (end < 0)
				end = (int)pos + 1;
			break;
		}
		if ((l & 0xc0) == 0xc0) {
			if (pos + 1 >= len || ++jumps > 16)
				return -1;
			if (end < 0)
				end = (int)pos + 2;
			pos = (size_t)((l & 0x3f) << 8 | pkt[pos + 1]);
			continue;
		}
		if (l & 0xc0)
			return -1;
		if (pos + 1 + l > len || o + l + 2 > outn)
			return -1;
		if (o)
			out[o++] = '.';
		for (int i = 0; i < l; i++)
			out[o++] = (char)tolower(pkt[pos + 1 + i]);
		pos += 1 + (size_t)l;
	}
	out[o] = 0;
	return end;
}

/* Writes "a.b" as DNS labels. Returns bytes written or 0. */
static size_t write_name(uint8_t *out, size_t n, const char *name)
{
	size_t o = 0;
	const char *p = name;

	while (*p) {
		size_t l = strcspn(p, ".");

		if (l == 0 || l > 63 || o + l + 2 > n)
			return 0;
		out[o++] = (uint8_t)l;
		memcpy(out + o, p, l);
		o += l;
		p += l;
		if (*p == '.')
			p++;
	}
	if (o + 1 > n)
		return 0;
	out[o++] = 0;
	return o;
}

static int dns_reply(bool mdns, const uint8_t *q, size_t qlen, uint32_t ip, const char *host,
		     bool src_mdns, uint8_t *out, size_t outmax, bool *unicast)
{
	char want[80], name[256];
	uint16_t flags, qd;
	size_t off = 12, o, qstart = 0, qend = 0;
	bool match = false, qu = false;

	if (qlen < 12)
		return -1;
	flags = rd16(q + 2);
	if ((flags & 0x8000) || ((flags >> 11) & 0xf) != 0)
		return 0;                              /* a response, or not a query */
	qd = rd16(q + 4);
	if (qd == 0 || (!mdns && qd != 1))
		return 0;
	snprintf(want, sizeof(want), mdns ? "%s.local" : "%s", host);
	for (char *c = want; *c; c++)
		*c = (char)tolower((unsigned char)*c);

	for (int i = 0; i < qd && i < 16; i++) {
		int e = read_name(q, qlen, off, name, sizeof(name));
		uint16_t type, cls;

		if (e < 0 || (size_t)e + 4 > qlen)
			return -1;
		type = rd16(q + e);
		cls = rd16(q + e + 2);
		if (!strcmp(name, want) && (type == 1 || type == 255) && (cls & 0x7fff) == 1) {
			match = true;
			qu = (cls & 0x8000) != 0;
			qstart = off;
			qend = (size_t)e + 4;
			break;
		}
		off = (size_t)e + 4;
	}
	if (!match)
		return 0;

	/* legacy unicast mDNS (source port != 5353) and LLMNR echo the
	 * question and the ID; a normal mDNS response has ID 0, no question */
	bool echo = !mdns || !src_mdns;

	if (outmax < 12 + (qend - qstart) + 300)
		return -1;
	memset(out, 0, 12);
	if (echo)
		memcpy(out, q, 2);                     /* ID */
	wr16(out + 2, mdns ? 0x8400 : 0x8000);     /* QR (+ AA for mDNS) */
	wr16(out + 4, echo ? 1 : 0);
	wr16(out + 6, 1);
	o = 12;
	if (echo) {
		size_t w = write_name(out + o, outmax - o, want);

		if (!w)
			return -1;
		o += w;
		wr16(out + o, 1);                      /* QTYPE A */
		wr16(out + o + 2, 1);                  /* QCLASS IN */
		o += 4;
	}
	{
		size_t w = write_name(out + o, outmax - o, want);

		if (!w || o + w + 14 > outmax)
			return -1;
		o += w;
		wr16(out + o, 1);                      /* TYPE A */
		wr16(out + o + 2, (mdns && !echo) ? 0x8001 : 1);   /* cache-flush */
		wr32(out + o + 4, mdns ? (echo ? 10 : 120) : 30);  /* TTL */
		wr16(out + o + 8, 4);
		memcpy(out + o + 10, &ip, 4);          /* already network order */
		o += 14;
	}
	*unicast = echo || qu;
	return (int)o;
}

/* NetBIOS first-level encoding of a 16-byte name. */
static void nb_encode(const char *host, uint8_t suffix, uint8_t *out32)
{
	uint8_t raw[16];
	size_t l = strlen(host);

	memset(raw, ' ', 15);
	for (size_t i = 0; i < 15 && i < l; i++)
		raw[i] = (uint8_t)toupper((unsigned char)host[i]);
	raw[15] = suffix;
	for (int i = 0; i < 16; i++) {
		out32[2 * i] = (uint8_t)('A' + (raw[i] >> 4));
		out32[2 * i + 1] = (uint8_t)('A' + (raw[i] & 0xf));
	}
}

static int nbns_reply(const uint8_t *q, size_t qlen, uint32_t ip, const char *host,
		      uint8_t *out, size_t outmax, bool *unicast)
{
	uint8_t want[32];
	uint16_t flags;
	uint8_t suffix;
	bool ok = false;

	if (qlen < 12 + 34 + 4)
		return qlen < 12 ? -1 : 0;
	flags = rd16(q + 2);
	if ((flags & 0x8000) || ((flags >> 11) & 0xf) != 0 || rd16(q + 4) != 1)
		return 0;
	if (q[12] != 32 || q[12 + 33] != 0)
		return 0;                              /* scoped names: not ours */
	if (rd16(q + 46) != 0x0020 || rd16(q + 48) != 1)
		return 0;                              /* NB / IN only (not NBSTAT) */
	/* suffix = last encoded byte pair: two letters 'A'..'P' (review F-L10:
	 * any other byte made a negative left shift) */
	if (q[12 + 31] < 'A' || q[12 + 31] > 'P' || q[12 + 32] < 'A' || q[12 + 32] > 'P')
		return 0;
	suffix = (uint8_t)((unsigned)(q[12 + 31] - 'A') << 4 | (unsigned)(q[12 + 32] - 'A'));
	if (suffix == 0x00 || suffix == 0x20) {
		nb_encode(host, suffix, want);
		ok = !memcmp(q + 13, want, 32);
	}
	if (!ok)
		return 0;
	if (outmax < 12 + 34 + 16)
		return -1;
	memset(out, 0, 12);
	memcpy(out, q, 2);                          /* TRN_ID */
	wr16(out + 2, 0x8500);                      /* response, AA, RD */
	wr16(out + 6, 1);                           /* ANCOUNT */
	memcpy(out + 12, q + 12, 34);               /* RR_NAME */
	wr16(out + 46, 0x0020);                     /* NB */
	wr16(out + 48, 1);                          /* IN */
	wr32(out + 50, 300);                        /* TTL */
	wr16(out + 54, 6);                          /* RDLENGTH */
	wr16(out + 56, 0);                          /* NB_FLAGS: B-node, unique */
	memcpy(out + 58, &ip, 4);
	*unicast = true;
	return 62;
}

int netnames_reply(enum netnames_proto proto, const uint8_t *q, size_t qlen, uint32_t ip,
		   const char *hostname, bool src_port_is_mdns, uint8_t *out, size_t outmax,
		   bool *unicast)
{
	*unicast = false;
	switch (proto) {
	case NETNAMES_MDNS:
		return dns_reply(true, q, qlen, ip, hostname, src_port_is_mdns, out, outmax, unicast);
	case NETNAMES_LLMNR:
		return dns_reply(false, q, qlen, ip, hostname, false, out, outmax, unicast);
	case NETNAMES_NBNS:
		return nbns_reply(q, qlen, ip, hostname, out, outmax, unicast);
	}
	return -1;
}

/* ------------------------------------------------------------ service */

static struct {
	pthread_t th;
	bool thread;
	volatile int stop;
	int pipe_r, pipe_w;
	int fd[3];                 /* mdns, llmnr, nbns */
	uint16_t port[3];
	char host[64];
	bool no_multicast;
	/* IPv4 interfaces, refreshed every 10 s (join_groups): the replies'
	 * address and the on-link check come from here, not from a
	 * getifaddrs() per packet (review F-L4). joined: groups joined and
	 * announced on it; entries of interfaces that went away are dropped
	 * (review F-L6: 16 appearances used to fill the table for good). */
	struct nif {
		unsigned idx;
		uint32_t ip, mask;     /* network order */
		bool joined;
		bool seen;
	} ifs[16];
	int nifs;
	int64_t last_join;         /* the last join_groups() */
} N = { .pipe_r = -1, .pipe_w = -1, .fd = { -1, -1, -1 } };

void netnames_defaults(struct netnames_config *c)
{
	memset(c, 0, sizeof(*c));
	c->hostname = "retrostone";
	c->mdns = c->llmnr = c->nbns = true;
}

static int udp_socket(uint16_t port, bool bcast)
{
	struct sockaddr_in a;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0), one = 1;

	if (fd < 0)
		return -errno;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
	setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
	if (bcast)
		setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		int e = -errno;

		close(fd);
		return e;
	}
	return fd;
}

static struct nif *nif_find(unsigned ifindex)
{
	for (int k = 0; k < N.nifs; k++)
		if (N.ifs[k].idx == ifindex)
			return &N.ifs[k];
	return NULL;
}

static void send_on(int fd, const uint8_t *buf, size_t len, const struct sockaddr_in *to,
		    unsigned ifindex)
{
	char cbuf[CMSG_SPACE(sizeof(struct in_pktinfo))];
	struct iovec iov = { (void *)buf, len };
	struct msghdr mh = { .msg_name = (void *)to, .msg_namelen = sizeof(*to),
			     .msg_iov = &iov, .msg_iovlen = 1 };

	if (ifindex) {
		struct cmsghdr *c;
		struct in_pktinfo pi;

		memset(cbuf, 0, sizeof(cbuf));
		mh.msg_control = cbuf;
		mh.msg_controllen = sizeof(cbuf);
		c = CMSG_FIRSTHDR(&mh);
		c->cmsg_level = IPPROTO_IP;
		c->cmsg_type = IP_PKTINFO;
		c->cmsg_len = CMSG_LEN(sizeof(pi));
		memset(&pi, 0, sizeof(pi));
		pi.ipi_ifindex = (int)ifindex;
		memcpy(CMSG_DATA(c), &pi, sizeof(pi));
	}
	sendmsg(fd, &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void announce(unsigned ifindex, uint32_t ip)
{
	/* an unsolicited mDNS response: caches drop a stale address */
	uint8_t q[64], out[512];
	char name[80];
	size_t o = 12;
	bool uc;
	int n;
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353) };

	if (N.fd[0] < 0 || N.port[0] != 5353)
		return;
	memset(q, 0, sizeof(q));
	wr16(q + 4, 1);
	snprintf(name, sizeof(name), "%s.local", N.host);
	o += write_name(q + o, sizeof(q) - o - 4, name);
	wr16(q + o, 1);
	wr16(q + o + 2, 1);
	n = dns_reply(true, q, o + 4, ip, N.host, true, out, sizeof(out), &uc);
	if (n > 0) {
		inet_pton(AF_INET, MDNS_GROUP, &to.sin_addr);
		send_on(N.fd[0], out, (size_t)n, &to, ifindex);
	}
}

/* Refreshes the interface table; joins the groups on new interfaces and
 * announces the name on new or re-addressed ones. */
static void join_groups(void)
{
	struct ifaddrs *ifa = NULL, *i;

	if (getifaddrs(&ifa) < 0)
		return;
	for (int k = 0; k < N.nifs; k++)
		N.ifs[k].seen = false;
	for (i = ifa; i; i = i->ifa_next) {
		unsigned idx;
		uint32_t ip, mask;
		struct nif *f;
		bool fresh;

		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP))
			continue;
		idx = if_nametoindex(i->ifa_name);
		ip = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
		mask = i->ifa_netmask ? ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr : 0xffffffffu;
		if (!idx)
			continue;
		f = nif_find(idx);
		if (f && f->seen)
			continue;              /* a second address: the first one is used */
		if (!f) {
			if (N.nifs == (int)(sizeof(N.ifs) / sizeof(N.ifs[0])))
				continue;
			f = &N.ifs[N.nifs++];
			memset(f, 0, sizeof(*f));
			f->idx = idx;
		}
		fresh = f->ip != ip;
		f->ip = ip;
		f->mask = mask;
		f->seen = true;
		if (N.no_multicast || !(i->ifa_flags & IFF_MULTICAST) || (i->ifa_flags & IFF_LOOPBACK))
			continue;
		if (!f->joined) {
			for (int s = 0; s < 2; s++) {
				struct ip_mreqn m;

				if (N.fd[s] < 0)
					continue;
				memset(&m, 0, sizeof(m));
				inet_pton(AF_INET, s == 0 ? MDNS_GROUP : LLMNR_GROUP, &m.imr_multiaddr);
				m.imr_ifindex = (int)idx;
				if (setsockopt(N.fd[s], IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0 &&
				    errno != EADDRINUSE)
					tr_log("netnames: join on %s: %s", i->ifa_name, strerror(errno));
			}
			f->joined = true;
			fresh = true;
		}
		if (fresh)
			announce(idx, ip);
	}
	freeifaddrs(ifa);
	/* interfaces gone (their memberships went with them): forget them */
	for (int k = 0; k < N.nifs;)
		if (!N.ifs[k].seen)
			N.ifs[k] = N.ifs[--N.nifs];
		else
			k++;
}

static void serve(int which)
{
	uint8_t buf[1500], out[1500];
	char cbuf[256];

	for (;;) {
		struct sockaddr_in from;
		struct iovec iov = { buf, sizeof(buf) };
		struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof(from),
				     .msg_iov = &iov, .msg_iovlen = 1,
				     .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
		ssize_t n = recvmsg(N.fd[which], &mh, MSG_DONTWAIT);
		unsigned ifindex = 0;
		uint32_t ip = 0;
		bool uc = false;
		struct nif *f;
		int r;

		if (n < 0)
			return;
		for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
			if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
				struct in_pktinfo pi;

				memcpy(&pi, CMSG_DATA(c), sizeof(pi));
				ifindex = (unsigned)pi.ipi_ifindex;
				ip = pi.ipi_spec_dst.s_addr;
			}
		/* the interface's own address (cached), not spec_dst (a broadcast
		 * or group address for NBNS/mDNS queries) */
		f = ifindex ? nif_find(ifindex) : NULL;
		if (!f && ifindex && tr_now_ms() - N.last_join > 1000) {
			join_groups();          /* an interface we have not seen yet */
			N.last_join = tr_now_ms();
			f = nif_find(ifindex);
		}
		if (f)
			ip = f->ip;
		if (!ip)
			continue;
		r = netnames_reply(which == 0 ? NETNAMES_MDNS : which == 1 ? NETNAMES_LLMNR :
				   NETNAMES_NBNS, buf, (size_t)n, ip, N.host,
				   ntohs(from.sin_port) == 5353, out, sizeof(out), &uc);
		if (r <= 0)
			continue;
		/* unicast answers only to the local link (RFC 6762 §11, review
		 * F-L5): never a reflector for off-link sources */
		if (uc && (!f || ((from.sin_addr.s_addr ^ f->ip) & f->mask) != 0))
			continue;
		if (uc) {
			send_on(N.fd[which], out, (size_t)r, &from, 0);
		} else {
			struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(N.port[0]) };

			inet_pton(AF_INET, MDNS_GROUP, &to.sin_addr);
			send_on(N.fd[which], out, (size_t)r, &to, ifindex);
		}
	}
}

static void *thread_main(void *arg)
{
	(void)arg;
	N.last_join = tr_now_ms() - 20000;
	while (!N.stop) {
		struct pollfd pf[4];
		int np = 0, map[4];

		if (tr_now_ms() - N.last_join > 10000) {   /* WiFi may come up later */
			join_groups();
			N.last_join = tr_now_ms();
		}
		for (int i = 0; i < 3; i++)
			if (N.fd[i] >= 0) {
				pf[np].fd = N.fd[i];
				pf[np].events = POLLIN;
				map[np++] = i;
			}
		pf[np].fd = N.pipe_r;
		pf[np].events = POLLIN;
		map[np++] = -1;
		if (poll(pf, (nfds_t)np, 5000) <= 0)
			continue;
		for (int i = 0; i < np; i++) {
			if (!(pf[i].revents & POLLIN))
				continue;
			if (map[i] < 0)
				return NULL;
			serve(map[i]);
		}
	}
	return NULL;
}

int netnames_start(const struct netnames_config *c)
{
	struct netnames_config def;
	int fds[2], mc = 0;

	if (N.thread)
		return -EALREADY;
	if (!c) {
		netnames_defaults(&def);
		c = &def;
	}
	tr_strlcpy(N.host, c->hostname ? c->hostname : "retrostone", sizeof(N.host));
	N.no_multicast = c->no_multicast;
	N.port[0] = c->mdns_port ? c->mdns_port : 5353;
	N.port[1] = c->llmnr_port ? c->llmnr_port : 5355;
	N.port[2] = c->nbns_port ? c->nbns_port : 137;
	N.nifs = 0;
	for (int i = 0; i < 3; i++) {
		bool on = i == 0 ? c->mdns : i == 1 ? c->llmnr : c->nbns;

		N.fd[i] = on ? udp_socket(N.port[i], i == 2) : -1;
		if (on && N.fd[i] < 0) {
			tr_log("netnames: port %u: %s", N.port[i], strerror(-N.fd[i]));
			N.fd[i] = -1;
		} else if (on) {
			mc++;
		}
	}
	if (N.fd[0] >= 0) {
		unsigned char ttl = 255, loop = 0;

		setsockopt(N.fd[0], IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
		setsockopt(N.fd[0], IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
	}
	if (!mc)
		return -EADDRNOTAVAIL;
	if (pipe2(fds, O_CLOEXEC) < 0)
		goto fail;
	N.pipe_r = fds[0];
	N.pipe_w = fds[1];
	N.stop = 0;
	if (pthread_create(&N.th, NULL, thread_main, NULL) != 0)
		goto fail;
	N.thread = true;
	return 0;
fail:
	for (int i = 0; i < 3; i++)
		if (N.fd[i] >= 0) {
			close(N.fd[i]);
			N.fd[i] = -1;
		}
	if (N.pipe_r >= 0) {
		close(N.pipe_r);
		close(N.pipe_w);
		N.pipe_r = N.pipe_w = -1;
	}
	return -EAGAIN;
}

void netnames_stop(void)
{
	if (!N.thread)
		return;
	N.stop = 1;
	(void)!write(N.pipe_w, "x", 1);
	pthread_join(N.th, NULL);
	N.thread = false;
	for (int i = 0; i < 3; i++)
		if (N.fd[i] >= 0) {
			close(N.fd[i]);
			N.fd[i] = -1;
		}
	close(N.pipe_r);
	close(N.pipe_w);
	N.pipe_r = N.pipe_w = -1;
}

bool netnames_running(void)
{
	return N.thread;
}
