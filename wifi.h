/*
 * wifi.h — shared types, constants, and scan API
 *
 * Part of wifiscan — FreeBSD 14.4 wireless TUI scanner.
 */

#ifndef WIFI_H
#define WIFI_H

#include <net/if.h>
#include <net80211/ieee80211_ioctl.h>

#include <stdint.h>
#include <time.h>

/* ── tunables ────────────────────────────────────────────────────────────── */

/*
 * Hard ceiling for the scan-results buffer.
 * ieee80211req.i_len is int16_t (signed 16-bit).
 * INT16_MAX = 32767; anything larger wraps negative → EFAULT ("Bad address").
 */
#define WIFI_MAX_IELEN 32767

/* Maximum number of APs we track in memory. */
#define WIFI_MAX_APS 256

/* Default auto-refresh interval (seconds). */
#define WIFI_DEFAULT_REFRESH 3

/* RSSI history samples kept per AP for the signal graph. */
#define WIFI_RSSI_HISTORY 64

/* ── AP record ───────────────────────────────────────────────────────────── */

typedef struct {
	/* ── identity ── */
	char ssid[IEEE80211_NWID_LEN + 1];
	uint8_t bssid[6];

	/* ── radio ── */
	uint16_t freq; /* channel frequency in MHz              */
	uint8_t chan;  /* 802.11 channel number (from freq)     */
	int8_t rssi;   /* received signal strength (.5 dBm)     */
	int8_t noise;  /* noise floor (.5 dBm)                  */
	int snr;       /* signal-to-noise ratio (dB)            */

	/* ── 802.11 ── */
	uint8_t intval;	   /* beacon interval in TUs (1 TU=1.024ms) */
	uint16_t capinfo;  /* capability information field           */
	uint8_t erp;	   /* ERP information element value          */
	uint8_t nrates;	   /* number of supported basic rates        */
	uint8_t rates[15]; /* rates in 500 kbps units                */

	/* ── security ── */
	int wpa; /* 1 if WPA vendor IE found              */
	int rsn; /* 1 if RSN/WPA2 IE found                */

	/* ── PHY / band ── */
	int is_5ghz; /* freq >= 5000                          */
	int is_6ghz; /* freq >= 5925 (Wi-Fi 6E)               */
	int is_ht;   /* HT Capabilities IE present (11n)      */
	int is_vht;  /* VHT Capabilities IE present (11ac)    */
	int is_he;   /* HE Capabilities IE present (11ax)     */

	/* ── RSSI history ring buffer (for the signal graph) ── */
	int8_t rssi_hist[WIFI_RSSI_HISTORY];
	int hist_head;	/* next write slot                       */
	int hist_count; /* number of valid samples (0..WIFI_RSSI_HISTORY) */

	time_t last_seen;
} wifi_ap_t;

/* ── scan state ──────────────────────────────────────────────────────────── */

typedef struct {
	wifi_ap_t aps[WIFI_MAX_APS];
	int naps;
	time_t last_scan;
	int last_ok; /* 1 if the most recent scan succeeded   */
} wifi_state_t;

/* ── public API ──────────────────────────────────────────────────────────── */

int wifi_open_socket(void);
int wifi_scan(int sock, const char *ifname, wifi_state_t *state);
uint8_t wifi_freq_to_chan(uint16_t freq);
int wifi_ie_has_rsn(const uint8_t *ies, size_t len);
int wifi_ie_has_wpa(const uint8_t *ies, size_t len);

/* ── capability helpers (inline, no link cost) ───────────────────────────── */

static inline int
wifi_cap_ess(uint16_t c)
{
	return (c >> 0) & 1;
}
static inline int
wifi_cap_ibss(uint16_t c)
{
	return (c >> 1) & 1;
}
static inline int
wifi_cap_privacy(uint16_t c)
{
	return (c >> 4) & 1;
}
static inline int
wifi_cap_short_preamble(uint16_t c)
{
	return (c >> 5) & 1;
}
static inline int
wifi_cap_short_slot(uint16_t c)
{
	return (c >> 10) & 1;
}

/* ── band / PHY string helpers ───────────────────────────────────────────── */

static inline const char *
wifi_band_str(const wifi_ap_t *a)
{
	if (a->is_6ghz)
		return "6 GHz (Wi-Fi 6E)";
	if (a->is_5ghz)
		return "5 GHz";
	return "2.4 GHz";
}

static inline const char *
wifi_phy_str(const wifi_ap_t *a)
{
	if (a->is_he)
		return "802.11ax (Wi-Fi 6/6E)";
	if (a->is_vht)
		return "802.11ac (Wi-Fi 5)";
	if (a->is_ht)
		return "802.11n  (Wi-Fi 4)";
	if (a->is_5ghz)
		return "802.11a";
	return "802.11b/g";
}

static inline const char *
wifi_sec_str(const wifi_ap_t *a)
{
	if (a->rsn)
		return "WPA2/RSN (802.11i)";
	if (a->wpa)
		return "WPA (TKIP)";
	if (wifi_cap_privacy(a->capinfo))
		return "WEP (legacy)";
	return "Open (no encryption)";
}

#endif /* WIFI_H */
