/*
 * wifi.c — FreeBSD 14.4 net80211 scan implementation
 *
 * FreeBSD 14.4 ioctl notes (root causes of the common errors):
 *  • i_len is int16_t → cap buffer at WIFI_MAX_IELEN (32767).
 *    Values > INT16_MAX wrap negative → kernel returns EFAULT.
 *  • Scan trigger: SIOCG80211 + IEEE80211_IOC_SCAN_REQ (NOT SIOCS80211).
 *    SIOCS80211 returns EINVAL on FreeBSD 14.
 *  • isr_chan removed — derive channel from isr_freq.
 *  • isr_beacon_int renamed to isr_intval.
 *  • Variable data starts at isr_ie_off (not sizeof(*sr)):
 *    [isr_ssid_len bytes: SSID][isr_meshid_len bytes: MeshID][IEs]
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net80211/ieee80211_ioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wifi.h"

/* ── IE element IDs we care about ────────────────────────────────────────── */
#define IE_SUPPORTED_RATES 1
#define IE_RSN		   48
#define IE_HT_CAP	   45
#define IE_VHT_CAP	   191
#define IE_VENDOR_SPECIFIC 221
/* HE (802.11ax) uses an extension tag (ID 255, ext tag 35) */
#define IE_EXTENSION  255
#define IE_EXT_HE_CAP 35

/* ── IE walking ──────────────────────────────────────────────────────────── */

static const uint8_t *
find_ie(const uint8_t *ies, size_t len, uint8_t id, size_t *out_len)
{
	size_t pos = 0;
	while (pos + 2 <= len) {
		uint8_t eid = ies[pos];
		uint8_t elen = ies[pos + 1];
		if (pos + 2 + elen > len)
			break;
		if (eid == id) {
			if (out_len)
				*out_len = elen;
			return ies + pos + 2;
		}
		pos += 2 + elen;
	}
	return NULL;
}

int
wifi_ie_has_rsn(const uint8_t *ies, size_t len)
{
	size_t l;
	return find_ie(ies, len, IE_RSN, &l) != NULL;
}

int
wifi_ie_has_wpa(const uint8_t *ies, size_t len)
{
	size_t pos = 0;
	while (pos + 2 <= len) {
		uint8_t eid = ies[pos];
		uint8_t elen = ies[pos + 1];
		if (pos + 2 + elen > len)
			break;
		if (eid == IE_VENDOR_SPECIFIC && elen >= 4) {
			const uint8_t *p = ies + pos + 2;
			/* OUI 00:50:F2, type 0x01 = WPA Information Element */
			if (p[0] == 0x00 && p[1] == 0x50 && p[2] == 0xF2 &&
			    p[3] == 0x01)
				return 1;
		}
		pos += 2 + elen;
	}
	return 0;
}

static int
ie_has_ht(const uint8_t *ies, size_t len)
{
	size_t l;
	return find_ie(ies, len, IE_HT_CAP, &l) != NULL;
}

static int
ie_has_vht(const uint8_t *ies, size_t len)
{
	size_t l;
	return find_ie(ies, len, IE_VHT_CAP, &l) != NULL;
}

static int
ie_has_he(const uint8_t *ies, size_t len)
{
	/* HE Capabilities: ID=255, first payload byte = extension tag 35 */
	size_t pos = 0;
	while (pos + 2 <= len) {
		uint8_t eid = ies[pos];
		uint8_t elen = ies[pos + 1];
		if (pos + 2 + elen > len)
			break;
		if (eid == IE_EXTENSION && elen >= 1 &&
		    ies[pos + 2] == IE_EXT_HE_CAP)
			return 1;
		pos += 2 + elen;
	}
	return 0;
}

/* ── channel derivation ──────────────────────────────────────────────────── */

uint8_t
wifi_freq_to_chan(uint16_t freq)
{
	if (freq == 2484)
		return 14;
	if (freq < 2484)
		return (uint8_t)((freq - 2407) / 5);
	if (freq < 5950)
		return (uint8_t)(freq / 5 - 1000); /* 5 GHz   */
	return (uint8_t)((freq - 5950) / 5 + 1);   /* 6 GHz   */
}

/* ── variable-data accessors ─────────────────────────────────────────────── */

static const uint8_t *
sr_vardata(const struct ieee80211req_scan_result *sr)
{
	return (const uint8_t *)sr + sr->isr_ie_off;
}

static const uint8_t *
sr_ies_ptr(const struct ieee80211req_scan_result *sr)
{
	return sr_vardata(sr) + sr->isr_ssid_len + sr->isr_meshid_len;
}

static void
sr_get_ssid(const struct ieee80211req_scan_result *sr, char *buf, size_t buflen)
{
	size_t n = sr->isr_ssid_len;
	if (n == 0 || n > IEEE80211_NWID_LEN || n >= buflen) {
		buf[0] = '\0';
		return;
	}
	memcpy(buf, sr_vardata(sr), n);
	buf[n] = '\0';
}

/* ── socket ──────────────────────────────────────────────────────────────── */

int
wifi_open_socket(void)
{
	int s = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (s < 0)
		perror("socket");
	return s;
}

/* ── scan trigger ────────────────────────────────────────────────────────── */

static int
trigger_scan(int sock, const char *ifname)
{
	struct ieee80211req ireq;
	struct ieee80211_scan_req sr;

	memset(&sr, 0, sizeof(sr));
	memset(&ireq, 0, sizeof(ireq));
	strlcpy(ireq.i_name, ifname, sizeof(ireq.i_name));

	ireq.i_type = IEEE80211_IOC_SCAN_REQ;
	sr.sr_flags = IEEE80211_IOC_SCAN_ACTIVE | IEEE80211_IOC_SCAN_ONCE |
	    IEEE80211_IOC_SCAN_NOPICK;
	sr.sr_duration = IEEE80211_IOC_SCAN_FOREVER;
	sr.sr_nssid = 0;
	ireq.i_data = (void *)&sr;
	ireq.i_len = (int16_t)sizeof(sr);

	/* Must use SIOCG80211, not SIOCS80211 — FreeBSD 14 requirement */
	return ioctl(sock, SIOCG80211, &ireq);
}

/* ── result fetch & merge ────────────────────────────────────────────────── */

int
wifi_scan(int sock, const char *ifname, wifi_state_t *state)
{
	trigger_scan(sock, ifname); /* best-effort; ignore failure */

	struct timespec ts = { 0, 300000000L }; /* 300 ms */
	nanosleep(&ts, NULL);

	struct ieee80211req ireq;
	uint8_t *buf = malloc(WIFI_MAX_IELEN);
	if (!buf) {
		perror("malloc");
		return -1;
	}

	memset(&ireq, 0, sizeof(ireq));
	strlcpy(ireq.i_name, ifname, sizeof(ireq.i_name));
	ireq.i_type = IEEE80211_IOC_SCAN_RESULTS;
	ireq.i_data = (void *)buf;
	ireq.i_len = WIFI_MAX_IELEN; /* int16_t — no overflow */

	if (ioctl(sock, SIOCG80211, &ireq) < 0) {
		perror("SIOCG80211 (scan results)");
		free(buf);
		state->last_ok = 0;
		return -1;
	}

	if (ireq.i_len <= 0) {
		free(buf);
		state->last_ok = 0;
		return -1;
	}

	size_t total = (size_t)ireq.i_len;
	size_t offset = 0;
	time_t now = time(NULL);

	while (offset + sizeof(struct ieee80211req_scan_result) <= total) {
		const struct ieee80211req_scan_result *sr =
		    (const struct ieee80211req_scan_result *)(buf + offset);

		if (sr->isr_len == 0 || offset + sr->isr_len > total)
			break;

		char ssid[IEEE80211_NWID_LEN + 1];
		sr_get_ssid(sr, ssid, sizeof(ssid));

		const uint8_t *ies = sr_ies_ptr(sr);
		size_t ie_len = sr->isr_ie_len;

		/* Find or allocate AP slot by BSSID */
		int idx = -1;
		for (int i = 0; i < state->naps; i++) {
			if (memcmp(state->aps[i].bssid, sr->isr_bssid, 6) ==
			    0) {
				idx = i;
				break;
			}
		}
		if (idx < 0) {
			if (state->naps >= WIFI_MAX_APS) {
				offset += sr->isr_len;
				continue;
			}
			idx = state->naps++;
			memset(&state->aps[idx], 0, sizeof(wifi_ap_t));
		}

		wifi_ap_t *a = &state->aps[idx];

		/* Identity */
		strlcpy(a->ssid, ssid, sizeof(a->ssid));
		memcpy(a->bssid, sr->isr_bssid, 6);

		/* Radio */
		a->freq = sr->isr_freq;
		a->chan = wifi_freq_to_chan(sr->isr_freq);
		a->rssi = sr->isr_rssi;
		a->noise = sr->isr_noise;
		a->snr = (int)sr->isr_rssi - (int)sr->isr_noise;

		/* 802.11 */
		a->intval = sr->isr_intval; /* was isr_beacon_int */
		a->capinfo = sr->isr_capinfo;
		a->erp = sr->isr_erp;
		a->nrates = sr->isr_nrates < 15 ? sr->isr_nrates : 15;
		memcpy(a->rates, sr->isr_rates, a->nrates);

		/* Security */
		a->rsn = (ie_len > 0) ? wifi_ie_has_rsn(ies, ie_len) : 0;
		a->wpa = (ie_len > 0) ? wifi_ie_has_wpa(ies, ie_len) : 0;

		/* PHY / band */
		a->is_5ghz = (sr->isr_freq >= 5000);
		a->is_6ghz = (sr->isr_freq >= 5925);
		a->is_ht = (ie_len > 0) ? ie_has_ht(ies, ie_len) : 0;
		a->is_vht = (ie_len > 0) ? ie_has_vht(ies, ie_len) : 0;
		a->is_he = (ie_len > 0) ? ie_has_he(ies, ie_len) : 0;

		/* RSSI history */
		a->rssi_hist[a->hist_head] = sr->isr_rssi;
		a->hist_head = (a->hist_head + 1) % WIFI_RSSI_HISTORY;
		if (a->hist_count < WIFI_RSSI_HISTORY)
			a->hist_count++;

		a->last_seen = now;
		offset += sr->isr_len;
	}

	free(buf);
	state->last_scan = now;
	state->last_ok = 1;
	return 0;
}
