/*
 * bt_ui.c - Settings > Network > Bluetooth devices: the paired devices
 * (connected or not) and, while the screen is open, the devices found
 * nearby. A pairs, trusts and connects a found device; on a paired one it
 * offers connect (or disconnect) and forget.
 *
 * Everything is done by the rsos-bt helper (ui->cfg.bt_helper), which drives
 * bluetoothctl; it runs as helper jobs (hw_job_start) and never blocks the
 * menu:
 *   rsos-bt scan on|off       when the screen opens (and every minute while
 *                             it is open) / closes
 *   rsos-bt list FILE         every BT_LIST_MS: "MAC\tstate\tname[\tkey=value,...]"
 *                             lines, state connected|paired|found; the extras
 *                             (unknown keys are ignored): battery=<percent>
 *   rsos-bt pair|connect|disconnect|forget MAC
 * Paired devices are trusted, so they reconnect by themselves when
 * Bluetooth comes on and when the pad is switched on (bluetoothd).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui_internal.h"

enum { K_BTDEV = 140 };
enum { ID_BT_HINT = 1, ID_BT_SEARCHING, ID_BT_DEV0 = 100 };
enum { ACT_NONE = 0, ACT_PAIR, ACT_CONNECT, ACT_DISCONNECT, ACT_FORGET };

#define BT_MAX 32
#define BT_LIST_MS 1500
#define BT_SCAN_MS 60000        /* scan on again (the helper's scan stops after 5 min) */

struct btdev {
	char mac[18];
	bool paired, connected;
	int battery;                    /* percent, -1 = unknown */
	char name[80];
};

static struct {
	struct menu *m;                 /* the screen, while it is open */
	struct btdev dev[BT_MAX];
	int n;
	char raw[4096];                 /* the last list read (rebuilt on change) */
	char path[64];                  /* the list file */
	bool listing;                   /* a list job runs */
	int act;                        /* the action that runs (ACT_*) */
	char act_mac[18], act_name[80];
	int64_t next_scan;
	char dlg_mac[18], dlg_name[80]; /* the device of the open dialog */
	bool dlg_connected;
} B;

bool bt_devices_available(const struct ui *ui)
{
	return ui->cfg.bt_helper && *ui->cfg.bt_helper && access(ui->cfg.bt_helper, X_OK) == 0;
}

static int run(struct ui *ui, const char *a1, const char *a2, int tag)
{
	const char *argv[] = { ui->cfg.bt_helper, a1, a2, NULL };

	return hw_job_start(argv, tag);
}

static void scan(struct ui *ui, bool on)
{
	if (run(ui, "scan", on ? "on" : "off", UI_JOB_BT_SCAN) < 0)
		LOGW("bt: cannot run %s", ui->cfg.bt_helper);
	if (on)
		B.next_scan = ui->now + BT_SCAN_MS;
}

/* the extras "key=value,key=value": the keys known here, others are ignored */
static void parse_extras(char *x, struct btdev *d)
{
	char *save = NULL;

	for (char *kv = strtok_r(x, ",", &save); kv; kv = strtok_r(NULL, ",", &save))
		if (!strncmp(kv, "battery=", 8) && kv[8] >= '0' && kv[8] <= '9' && atoi(kv + 8) <= 100)
			d->battery = atoi(kv + 8);
}

/* the line "MAC\tstate\tname[\tkey=value,...]" -> d */
static bool parse_line(char *l, struct btdev *d)
{
	char *st = strchr(l, '\t'), *name, *x = NULL;

	if (!st || st - l != 17)
		return false;
	*st++ = 0;
	name = strchr(st, '\t');
	if (name)
		*name++ = 0;
	memset(d, 0, sizeof(*d));
	d->battery = -1;
	if (name && (x = strchr(name, '\t')))
		*x++ = 0;
	strlcpy_(d->mac, l, sizeof(d->mac));
	d->connected = !strcmp(st, "connected");
	d->paired = d->connected || !strcmp(st, "paired");
	if (!d->paired && strcmp(st, "found"))
		return false;
	if (x)
		parse_extras(x, d);
	strlcpy_(d->name, name && *name ? name : l, sizeof(d->name));
	return true;
}

static void parse(const char *raw)
{
	char *copy = strdup(raw), *save = NULL;

	B.n = 0;
	if (!copy)
		return;
	for (char *l = strtok_r(copy, "\n", &save); l && B.n < BT_MAX; l = strtok_r(NULL, "\n", &save))
		if (parse_line(l, &B.dev[B.n]))
			B.n++;
	free(copy);
}

static bool selectable(const struct menu_item *it)
{
	return it->type != MI_HEADER && !(it->type == MI_INFO && it->disabled);
}

static void items(struct ui *ui, struct menu *m)
{
	struct menu_item *it;
	bool any_found = false;

	(void)ui;
	m->n = 0;
	/* TRANSLATORS: Settings > Network > Bluetooth devices: the hint at the
	 * top (one line, keep it short) */
	it = menu_add(m, MI_INFO, ID_BT_HINT, _("Put the controller in pairing mode"));
	it->disabled = true;
	for (int pass = 0; pass < 2; pass++) {
		bool header = false;

		for (int i = 0; i < B.n; i++) {
			const struct btdev *d = &B.dev[i];

			if (d->paired != (pass == 0))
				continue;
			if (!header) {
				/* TRANSLATORS: Bluetooth devices: list headings */
				menu_add(m, MI_HEADER, 0, pass == 0 ? _("Paired") : _("Found nearby"));
				header = true;
			}
			any_found |= pass == 1;
			it = menu_add(m, MI_ACTION, ID_BT_DEV0 + i, d->name);
			if (B.act && !strcmp(B.act_mac, d->mac))
				/* TRANSLATORS: Bluetooth devices: the value right of a device */
				strlcpy_(it->value, B.act == ACT_PAIR ? _("pairing...") :
					 B.act == ACT_CONNECT ? _("connecting...") : _("please wait..."),
					 sizeof(it->value));
			else if (d->connected && d->battery >= 0)
				/* TRANSLATORS: Bluetooth devices: connected pad and its
				 * battery level; %d is a percentage (keep %%) */
				snprintf(it->value, sizeof(it->value), _("connected, %d%%"), d->battery);
			else if (d->connected)
				strlcpy_(it->value, _("connected"), sizeof(it->value));
		}
	}
	if (!any_found) {
		menu_add(m, MI_HEADER, 0, _("Found nearby"));
		/* TRANSLATORS: Bluetooth devices: no device found yet, the scan goes on */
		it = menu_add(m, MI_INFO, ID_BT_SEARCHING, _("Searching..."));
		it->disabled = true;
	}
}

/* again, the cursor kept on the same device */
static void rebuild(struct ui *ui, struct menu *m, const char *cur_mac)
{
	int keep = m->cursor;

	items(ui, m);
	m->cursor = -1;
	for (int i = 0; cur_mac && *cur_mac && i < m->n; i++) {
		int k = m->items[i].id - ID_BT_DEV0;

		if (k >= 0 && k < B.n && !strcmp(B.dev[k].mac, cur_mac))
			m->cursor = i;
	}
	if (m->cursor < 0)
		m->cursor = MIN(keep, m->n - 1);
	for (int k = 0; k < m->n && !selectable(&m->items[m->cursor]); k++)
		m->cursor = (m->cursor + 1) % m->n;
	m->top = MIN(m->top, m->cursor);
	ui->dirty = true;
}

static const struct btdev *cursor_dev(struct menu *m)
{
	int k;

	if (m->cursor < 0 || m->cursor >= m->n)
		return NULL;
	k = m->items[m->cursor].id - ID_BT_DEV0;
	return k >= 0 && k < B.n ? &B.dev[k] : NULL;
}

static void refresh(struct ui *ui, struct menu *m)
{
	(void)m;
	if (!B.listing && run(ui, "list", B.path, UI_JOB_BT_LIST) > 0)
		B.listing = true;
	if (!B.act && ui->now >= B.next_scan)
		scan(ui, true);
}

static void start_act(struct ui *ui, int act, const char *mac, const char *name)
{
	static const char *const verbs[] = { NULL, "pair", "connect", "disconnect", "forget" };

	if (B.act) {
		ui_toastf(ui, "%s", _("please wait..."));
		return;
	}
	if (run(ui, verbs[act], mac, UI_JOB_BT_ACT) < 0) {
		ui_toastf(ui, _("Cannot run %s"), ui->cfg.bt_helper);
		return;
	}
	B.act = act;
	strlcpy_(B.act_mac, mac, sizeof(B.act_mac));
	strlcpy_(B.act_name, name, sizeof(B.act_name));
	if (act == ACT_PAIR)
		/* TRANSLATORS: toast; %s is the controller's name */
		ui_toastf(ui, _("Pairing %s..."), name);
	if (B.m)
		rebuild(ui, B.m, mac);
}

static void paired_choice(struct ui *ui, int choice, void *user)
{
	(void)user;
	if (choice == 0)
		start_act(ui, B.dlg_connected ? ACT_DISCONNECT : ACT_CONNECT, B.dlg_mac, B.dlg_name);
	else if (choice == 1)
		start_act(ui, ACT_FORGET, B.dlg_mac, B.dlg_name);
}

static void item(struct ui *ui, struct menu *m, struct menu_item *it, int dir)
{
	const struct btdev *d = cursor_dev(m);
	const char *buttons[4];

	(void)it;
	if (dir || !d)
		return;
	if (!d->paired) {
		start_act(ui, ACT_PAIR, d->mac, d->name);
		return;
	}
	strlcpy_(B.dlg_mac, d->mac, sizeof(B.dlg_mac));
	strlcpy_(B.dlg_name, d->name, sizeof(B.dlg_name));
	B.dlg_connected = d->connected;
	/* TRANSLATORS: dialog buttons for a paired Bluetooth controller,
	 * uppercase, short */
	buttons[0] = d->connected ? _("DISCONNECT") : _("CONNECT");
	buttons[1] = _("FORGET");
	buttons[2] = _("CANCEL");
	buttons[3] = NULL;
	dialog_open(ui, d->name, buttons, paired_choice, NULL);
}

static void destroy(struct ui *ui, struct menu *m)
{
	(void)m;
	B.m = NULL;
	scan(ui, false);
	if (B.path[0])
		unlink(B.path);
}

void bt_devices_open(struct ui *ui)
{
	/* TRANSLATORS: Settings > Network: the Bluetooth pairing screen */
	struct menu *m = menu_new(ui, _("Bluetooth devices"), K_BTDEV);

	snprintf(B.path, sizeof(B.path), "/tmp/rsos-bt-%d.list", (int)getpid());
	B.raw[0] = 0;
	B.n = 0;
	items(ui, m);
	m->on_item = item;
	m->on_refresh = refresh;
	m->on_destroy = destroy;
	m->refresh_every = BT_LIST_MS;
	B.m = m;
	scan(ui, true);
	menu_open(ui, m);
}

static void act_done(struct ui *ui, const struct hw_job_result *r)
{
	int act = B.act;

	B.act = 0;
	if (r->status != 0)
		LOGW("bt: %s: %s", B.act_mac, r->out);
	/* shown at once, before the next list */
	for (int i = 0; r->status == 0 && i < B.n; i++) {
		struct btdev *d = &B.dev[i];

		if (strcmp(d->mac, B.act_mac))
			continue;
		if (act == ACT_FORGET) {
			memmove(d, d + 1, (size_t)(B.n - i - 1) * sizeof(*d));
			B.n--;
		} else {
			d->paired = true;
			d->connected = act != ACT_DISCONNECT;
		}
		break;
	}
	B.raw[0] = 0;
	switch (act) {
	case ACT_PAIR:
	case ACT_CONNECT:
		if (r->status == 0)
			/* TRANSLATORS: toast; %s is the controller's name */
			ui_toastf(ui, _("%s connected"), B.act_name);
		else if (act == ACT_PAIR && (strstr(r->out, "Authentication") || strstr(r->out, "PIN") ||
					     strstr(r->out, "asskey")))
			/* TRANSLATORS: message; pairing without a PIN failed (some pads,
			 * e.g. a PlayStation 3 pad, must be paired with a USB cable or
			 * need a PIN first) */
			message_open(ui, _("This controller could not pair without a PIN. Some controllers must be paired with a USB cable first."));
		else if (act == ACT_PAIR)
			message_open(ui, _("Could not pair the controller. Put it in pairing mode and try again."));
		else
			message_open(ui, _("Could not connect the controller. Switch it on and try again."));
		break;
	case ACT_FORGET:
		if (r->status == 0)
			/* TRANSLATORS: toast; %s is the controller's name */
			ui_toastf(ui, _("%s forgotten"), B.act_name);
		else
			ui_toastf(ui, "%s", _("failed"));
		break;
	case ACT_DISCONNECT:
		if (r->status != 0)
			ui_toastf(ui, "%s", _("failed"));
		break;
	}
	/* pair and connect stopped the scan */
	if (B.m) {
		scan(ui, true);
		rebuild(ui, B.m, B.act_mac);
	}
}

bool bt_job_done(struct ui *ui, const struct hw_job_result *r)
{
	switch (r->tag) {
	case UI_JOB_BT_SCAN:
		if (r->status != 0)
			LOGW("bt: scan: %s", r->out);
		return true;
	case UI_JOB_BT_ACT:
		act_done(ui, r);
		return true;
	case UI_JOB_BT_LIST:
		B.listing = false;
		if (B.m) {
			size_t len = 0;
			char *buf = file_read(B.path, &len);
			const struct btdev *d = cursor_dev(B.m);
			char cur[18] = "";

			if (d)
				strlcpy_(cur, d->mac, sizeof(cur));
			if (!buf)
				buf = strdup("");
			if (buf && strcmp(buf, B.raw)) {
				strlcpy_(B.raw, buf, sizeof(B.raw));
				parse(B.raw);
				rebuild(ui, B.m, cur);
			}
			free(buf);
		}
		return true;
	}
	return false;
}
