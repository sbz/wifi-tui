/*
 * tui.h — ncurses TUI context and public API
 *
 * Part of wifiscan — FreeBSD 14.4 wireless TUI scanner.
 */

#ifndef TUI_H
#define TUI_H

#include "wifi.h"

/* ── sort column indices ─────────────────────────────────────────────────── */
#define TUI_SORT_SSID  0
#define TUI_SORT_BSSID 1
#define TUI_SORT_CHAN  2
#define TUI_SORT_FREQ  3
#define TUI_SORT_RSSI  4
#define TUI_SORT_SEC   5
#define TUI_SORT_BCN   6
#define TUI_NCOLS      7

/* ── action codes ────────────────────────────────────────────────────────── */
typedef enum {
	TUI_ACTION_NONE = 0,
	TUI_ACTION_QUIT,
	TUI_ACTION_RESCAN,
	TUI_ACTION_DETAIL /* Enter pressed — show detail pane */
} tui_action_t;

/* ── view mode ───────────────────────────────────────────────────────────── */
typedef enum {
	TUI_VIEW_LIST = 0, /* main AP list                     */
	TUI_VIEW_DETAIL	   /* detail pane for selected AP      */
} tui_view_t;

/* ── TUI context ─────────────────────────────────────────────────────────── */
typedef struct {
	wifi_state_t *state;
	const char *ifname;
	int refresh;
	int sort_col;
	int sort_rev;
	int selected;
	int scroll;
	int show_help;
	tui_view_t view;
} tui_ctx_t;

/* ── public API ──────────────────────────────────────────────────────────── */

void tui_init(tui_ctx_t *ctx, wifi_state_t *state, const char *ifname,
    int refresh_secs);
void tui_exit(void);
void tui_sort(tui_ctx_t *ctx);
void tui_redraw(tui_ctx_t *ctx);
tui_action_t tui_handle_key(tui_ctx_t *ctx, int ch);

#endif /* TUI_H */
