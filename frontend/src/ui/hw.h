/*
 * hw.h - small system helpers for the settings screens: battery, storage,
 * /boot/rsos.env overlays, WiFi credentials and asynchronous helper
 * processes (rsos-net), which never block the UI.
 */
#ifndef RSOS_UI_HW_H
#define RSOS_UI_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hw_battery {
	bool present;
	int percent;            /* -1 unknown */
	bool charging;
	bool ac;                /* external power connected */
	char status[24];        /* "Charging", "Discharging", "Full", ... */
};

/* Reads <ps_dir>/<supply>/{type,capacity,status,online}. */
void hw_battery(const char *ps_dir, struct hw_battery *b);
bool hw_storage(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);
void hw_version(const char *fallback, char *out, size_t n);

/* /boot/rsos.env "overlays=emmc sata" */
bool hw_env_has_overlay(const char *env_path, const char *name);
/* Adds/removes one overlay, keeping every other line. Remounts the root
 * filesystem read-write for the write if it is read-only. 0 or -errno. */
int hw_env_set_overlay(const char *env_path, const char *name, bool on);

/* Writes a wpa_supplicant.conf for one network (psk "" = open network). */
int hw_write_wpa(const char *path, const char *ssid, const char *psk);
/* Reads back the SSID of that file ("" if none). */
void hw_read_wpa_ssid(const char *path, char *ssid, size_t n);

/* --------------------------------------------------------------- jobs */
#define HW_MAX_JOBS 8

struct hw_job_result {
	int id;
	int tag;                /* caller's tag */
	int status;             /* exit code, or -signal, or -1000 - errno (exec failed) */
	char out[256];          /* first bytes of stdout+stderr, trimmed */
};

/* fork+exec argv[0] (absolute path) with stdout/stderr captured. Returns a
 * job id (> 0) or -errno. */
int hw_job_start(const char *const argv[], int tag);
/* Non-blocking: returns one finished job, if any. */
bool hw_job_poll(struct hw_job_result *r);
int hw_jobs_running(void);

/* True if a non-loopback interface has an IPv4 address (copied to addr). */
bool hw_has_ipv4(char *addr, size_t n);

#endif
