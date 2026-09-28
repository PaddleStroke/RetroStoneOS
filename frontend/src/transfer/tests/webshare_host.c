/*
 * webshare_host.c - runs the web share (and optionally the name responder)
 * outside the frontend, for host testing and debugging on the device:
 *
 *   rsos-webshare-host --root /tmp/data --port 8080 [--pin 123456]
 *                      [--idle SECONDS] [--names [--names-base PORT]] [--qr]
 *                      [--public]
 *
 * Prints the URLs and the PIN, optionally the QR code as text, then serves
 * until SIGINT/SIGTERM (or the idle timeout).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "transfer.h"

static volatile sig_atomic_t g_quit;

static void on_sig(int s)
{
	(void)s;
	g_quit = 1;
}

static void print_qr(const char *text)
{
	struct qr_code q;

	if (qr_encode(text, &q) < 0) {
		printf("(text too long for a QR code)\n");
		return;
	}
	printf("QR version %d, %dx%d, mask %d, for \"%s\":\n", q.version, q.size, q.size, q.mask,
	       text);
	/* two module rows per text line, 4-module quiet zone; dark = black
	 * text on a light terminal would be inverted, so print light modules
	 * as full blocks (scan it from a dark terminal) */
	for (int y = -4; y < q.size + 4; y += 2) {
		for (int x = -4; x < q.size + 4; x++) {
			int top = (y >= 0 && y < q.size && x >= 0 && x < q.size) ? q.m[y][x] : 0;
			int bot = (y + 1 >= 0 && y + 1 < q.size && x >= 0 && x < q.size) ? q.m[y + 1][x] : 0;

			fputs(!top && !bot ? "█" : !top ? "▀" : !bot ? "▄" : " ", stdout);
		}
		putchar('\n');
	}
}

int main(int argc, char **argv)
{
	struct webshare_config cfg = { 0 };
	struct webshare_status st;
	bool names = false, qr = false;
	int names_base = 0, r;

	cfg.data_root = "/data";
	cfg.port = 8080;
	cfg.idle_timeout_s = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "--root") && v)
			cfg.data_root = argv[++i];
		else if (!strcmp(a, "--port") && v)
			cfg.port = (uint16_t)atoi(argv[++i]);
		else if (!strcmp(a, "--pin") && v)
			cfg.pin = argv[++i];
		else if (!strcmp(a, "--idle") && v)
			cfg.idle_timeout_s = atoi(argv[++i]);
		else if (!strcmp(a, "--host") && v)
			cfg.hostname = argv[++i];
		else if (!strcmp(a, "--names"))
			names = true;
		else if (!strcmp(a, "--names-base") && v)
			names_base = atoi(argv[++i]);
		else if (!strcmp(a, "--qr"))
			qr = true;
		else if (!strcmp(a, "--public"))
			cfg.allow_public_peers = true;
		else {
			fprintf(stderr, "usage: %s --root DIR [--port N] [--pin PIN] [--idle S] "
				"[--host NAME] [--names [--names-base PORT]] [--qr] [--public]\n", argv[0]);
			return 2;
		}
	}
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	r = webshare_start(&cfg);
	if (r < 0) {
		fprintf(stderr, "webshare_start: %s\n", strerror(-r));
		return 1;
	}
	if (names) {
		struct netnames_config nc;

		netnames_defaults(&nc);
		if (cfg.hostname)
			nc.hostname = cfg.hostname;
		if (names_base) {             /* unprivileged test ports, no groups */
			nc.mdns_port = (uint16_t)names_base;
			nc.llmnr_port = (uint16_t)(names_base + 2);
			nc.nbns_port = (uint16_t)(names_base + 4);
			nc.no_multicast = true;
		}
		r = netnames_start(&nc);
		if (r < 0)
			fprintf(stderr, "netnames_start: %s\n", strerror(-r));
	}
	webshare_get_status(&st);
	printf("PIN %s\n", st.pin);
	for (int i = 0; i < st.nurls; i++)
		printf("URL %s\n", st.urls[i]);
	printf("URL %s\n", st.mdns_url);
	if (qr && st.qr_text[0])
		print_qr(st.qr_text);
	fflush(stdout);

	while (!g_quit && webshare_running())
		usleep(100 * 1000);
	webshare_get_status(&st);
	netnames_stop();
	webshare_stop();
	printf("stopped: %d file(s) received, %d deleted, %llu bytes, %d wrong PIN(s)%s%s\n",
	       st.files_received, st.files_deleted, (unsigned long long)st.bytes_received,
	       st.auth_failures, st.stop_reason[0] ? ", reason: " : "", st.stop_reason);
	{
		char ch[TRANSFER_SYS_MAX][TRANSFER_SYSID_MAX];
		int n = webshare_take_changes(ch, TRANSFER_SYS_MAX);

		printf("changed:");
		for (int i = 0; i < n; i++)
			printf(" %s", ch[i]);
		printf("\n");
	}
	return 0;
}
