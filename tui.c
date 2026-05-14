/*
 * tui.c — ncurses TUI: AP list view + detail pane + ASCII signal graph
 *
 * Views:
 *   LIST   — scrollable table of all APs (default)
 *   DETAIL — full-screen detail pane for the selected AP, opened with Enter,
 *             closed with Escape / q / Enter again.
 *
 * Detail pane sections:
 *   ┌─ Identity ──────────────────────────────────────────────┐
 *   │ SSID, BSSID, Band, PHY mode, Channel, Frequency        │
 *   ├─ Signal ────────────────────────────────────────────────┤
 *   │ RSSI, Noise, SNR, live bar, ASCII history graph        │
 *   ├─ 802.11 ────────────────────────────────────────────────┤
 *   │ Beacon interval, Capabilities decoded, ERP, Rates      │
 *   ├─ Security ──────────────────────────────────────────────┤
 *   │ Security mode, Privacy bit, Short-preamble, Short-slot │
 *   └─────────────────────────────────────────────────────────┘
 */

#include <math.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tui.h"
#include "wifi.h"

/* ── colour pair IDs ─────────────────────────────────────────────────────── */
#define COL_HEADER    1
#define COL_SELECTED  2
#define COL_RSSI_HI   3
#define COL_RSSI_MED  4
#define COL_RSSI_LO   5
#define COL_SEC_OPEN  6
#define COL_SEC_ENC   7
#define COL_TITLE     8
#define COL_STATUS    9
#define COL_BORDER    10
#define COL_DIM	      11
#define COL_COUNTDOWN 12
#define COL_LABEL     13 /* section label in detail pane   */
#define COL_VALUE     14 /* value text in detail pane      */
#define COL_GRAPH_HI  15 /* graph bar: strong              */
#define COL_GRAPH_MED 16 /* graph bar: medium              */
#define COL_GRAPH_LO  17 /* graph bar: weak                */
#define COL_SECTION   18 /* section header bar             */

/* ── column layout (list view) ──────────────────────────────────────────── */
static const char *col_names[TUI_NCOLS] = { "SSID", "BSSID", "CH", "FREQ",
	"RSSI", "SEC", "BCN" };
static const int col_widths[TUI_NCOLS] = { 24, 18, 4, 7, 6, 9, 5 };

/* ── sort ────────────────────────────────────────────────────────────────── */
static int g_cmp_col;
static int g_cmp_rev;

static int
ap_cmp(const void *a, const void *b)
{
	const wifi_ap_t *x = (const wifi_ap_t *)a;
	const wifi_ap_t *y = (const wifi_ap_t *)b;
	int r = 0;
	switch (g_cmp_col) {
	case TUI_SORT_SSID:
		r = strncmp(x->ssid, y->ssid, sizeof(x->ssid));
		break;
	case TUI_SORT_BSSID:
		r = memcmp(x->bssid, y->bssid, 6);
		break;
	case TUI_SORT_CHAN:
		r = (int)x->chan - (int)y->chan;
		break;
	case TUI_SORT_FREQ:
		r = (int)x->freq - (int)y->freq;
		break;
	case TUI_SORT_RSSI:
		r = (int)x->rssi - (int)y->rssi;
		break;
	case TUI_SORT_SEC: {
		int sx = x->rsn ? 2 : (x->wpa ? 1 : 0);
		int sy = y->rsn ? 2 : (y->wpa ? 1 : 0);
		r = sx - sy;
		break;
	}
	case TUI_SORT_BCN:
		r = (int)x->intval - (int)y->intval;
		break;
	}
	return g_cmp_rev ? -r : r;
}

/* ── init / teardown ─────────────────────────────────────────────────────── */

void
tui_init(tui_ctx_t *ctx, wifi_state_t *state, const char *ifname,
    int refresh_secs)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state = state;
	ctx->ifname = ifname;
	ctx->refresh = refresh_secs;
	ctx->sort_col = TUI_SORT_RSSI;
	ctx->sort_rev = 1;
	ctx->view = TUI_VIEW_LIST;

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	timeout(250);

	if (has_colors()) {
		start_color();
		use_default_colors();

		init_pair(COL_HEADER, COLOR_BLACK, COLOR_CYAN);
		init_pair(COL_SELECTED, COLOR_BLACK, COLOR_WHITE);
		init_pair(COL_RSSI_HI, COLOR_GREEN, -1);
		init_pair(COL_RSSI_MED, COLOR_YELLOW, -1);
		init_pair(COL_RSSI_LO, COLOR_RED, -1);
		init_pair(COL_SEC_OPEN, COLOR_RED, -1);
		init_pair(COL_SEC_ENC, COLOR_GREEN, -1);
		init_pair(COL_TITLE, COLOR_BLACK, COLOR_BLUE);
		init_pair(COL_STATUS, COLOR_BLACK, COLOR_CYAN);
		init_pair(COL_BORDER, COLOR_CYAN, -1);
		init_pair(COL_DIM, COLOR_WHITE, COLOR_BLACK);
		init_pair(COL_COUNTDOWN, COLOR_YELLOW, COLOR_CYAN);
		init_pair(COL_LABEL, COLOR_CYAN, -1);
		init_pair(COL_VALUE, COLOR_WHITE, -1);
		init_pair(COL_GRAPH_HI, COLOR_GREEN, -1);
		init_pair(COL_GRAPH_MED, COLOR_YELLOW, -1);
		init_pair(COL_GRAPH_LO, COLOR_RED, -1);
		init_pair(COL_SECTION, COLOR_BLACK, COLOR_BLUE);
	}
}

void
tui_exit(void)
{
	endwin();
}

/* ── sort ────────────────────────────────────────────────────────────────── */

void
tui_sort(tui_ctx_t *ctx)
{
	g_cmp_col = ctx->sort_col;
	g_cmp_rev = ctx->sort_rev;
	qsort(ctx->state->aps, ctx->state->naps, sizeof(wifi_ap_t), ap_cmp);
}

/* =========================================================================
 * SHARED drawing helpers
 * ====================================================================== */

static void
draw_title(const tui_ctx_t *ctx, int cols)
{
	attron(COLOR_PAIR(COL_TITLE) | A_BOLD);
	mvhline(0, 0, ' ', cols);
	mvprintw(0, 2, " WIFISCAN  |  %s", ctx->ifname);
	if (ctx->view == TUI_VIEW_DETAIL)
		mvprintw(0, 2 + 12 + (int)strlen(ctx->ifname), "  |  DETAIL");

	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char tbuf[12];
	strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
	mvprintw(0, cols - (int)strlen(tbuf) - 2, "%s", tbuf);
	attroff(COLOR_PAIR(COL_TITLE) | A_BOLD);
}

static void
draw_status(const tui_ctx_t *ctx, int rows, int cols)
{
	attron(COLOR_PAIR(COL_STATUS));
	mvhline(rows - 1, 0, ' ', cols);
	if (ctx->state->last_ok) {
		char tbuf[16];
		struct tm *tm = localtime(&ctx->state->last_scan);
		strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
		if (ctx->view == TUI_VIEW_LIST)
			mvprintw(rows - 1, 2, " %d AP%s  |  last scan %s",
			    ctx->state->naps, ctx->state->naps != 1 ? "s" : "",
			    tbuf);
		else
			mvprintw(rows - 1, 2,
			    " Enter:close  Esc/q:back  r:rescan");
	} else {
		mvprintw(rows - 1, 2,
		    " scan failed -- check interface/privileges");
	}

	time_t rem = (time_t)ctx->refresh -
	    (time(NULL) - ctx->state->last_scan);
	if (rem < 0)
		rem = 0;
	attroff(COLOR_PAIR(COL_STATUS));
	attron(COLOR_PAIR(COL_COUNTDOWN) | A_BOLD);
	mvprintw(rows - 1, cols - 18, " refresh in %2lds ", rem);
	attroff(COLOR_PAIR(COL_COUNTDOWN) | A_BOLD);
}

/* =========================================================================
 * LIST VIEW
 * ====================================================================== */

static void
draw_list_header(const tui_ctx_t *ctx, int cols)
{
	attron(COLOR_PAIR(COL_HEADER) | A_BOLD);
	mvhline(2, 0, ' ', cols);
	int x = 1;
	for (int c = 0; c < TUI_NCOLS; c++) {
		char label[32];
		if (c == ctx->sort_col)
			snprintf(label, sizeof(label), "%s%s", col_names[c],
			    ctx->sort_rev ? "v" : "^");
		else
			snprintf(label, sizeof(label), "%s", col_names[c]);
		mvprintw(2, x, "%-*s", col_widths[c], label);
		x += col_widths[c] + 1;
	}
	attroff(COLOR_PAIR(COL_HEADER) | A_BOLD);
}

static int
rssi_color(int rssi)
{
	if (rssi >= -60)
		return COL_RSSI_HI;
	if (rssi >= -75)
		return COL_RSSI_MED;
	return COL_RSSI_LO;
}

static void
draw_list_aps(tui_ctx_t *ctx, int rows, int cols)
{
	int top_row = 3;
	int max_rows = rows - top_row - 2;
	if (max_rows <= 0)
		return;

	if (ctx->selected < ctx->scroll)
		ctx->scroll = ctx->selected;
	if (ctx->selected >= ctx->scroll + max_rows)
		ctx->scroll = ctx->selected - max_rows + 1;

	for (int i = 0; i < max_rows; i++) {
		int idx = ctx->scroll + i;
		int row = top_row + i;

		if (idx >= ctx->state->naps) {
			mvhline(row, 0, ' ', cols);
			continue;
		}

		const wifi_ap_t *a = &ctx->state->aps[idx];
		int sel = (idx == ctx->selected);

		if (sel)
			attron(COLOR_PAIR(COL_SELECTED) | A_BOLD);
		mvhline(row, 0, ' ', cols);

		int x = 1;

		/* SSID */
		char sd[25];
		snprintf(sd, sizeof(sd), a->ssid[0] ? "%.24s" : "<hidden>",
		    a->ssid);
		mvprintw(row, x, "%-*s", col_widths[0], sd);
		x += col_widths[0] + 1;

		/* BSSID */
		mvprintw(row, x, "%02x:%02x:%02x:%02x:%02x:%02x", a->bssid[0],
		    a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4],
		    a->bssid[5]);
		x += col_widths[1] + 1;

		/* CH */
		mvprintw(row, x, "%-*u", col_widths[2], a->chan);
		x += col_widths[2] + 1;

		/* FREQ */
		char fb[10];
		snprintf(fb, sizeof(fb), "%uM", a->freq);
		mvprintw(row, x, "%-*s", col_widths[3], fb);
		x += col_widths[3] + 1;

		/* RSSI — always coloured */
		if (sel)
			attroff(COLOR_PAIR(COL_SELECTED) | A_BOLD);
		attron(COLOR_PAIR(rssi_color((int)a->rssi)));
		mvprintw(row, x, "%-*d", col_widths[4], (int)a->rssi);
		attroff(COLOR_PAIR(rssi_color((int)a->rssi)));
		if (sel)
			attron(COLOR_PAIR(COL_SELECTED) | A_BOLD);
		x += col_widths[4] + 1;

		/* SEC */
		if (sel)
			attroff(COLOR_PAIR(COL_SELECTED) | A_BOLD);
		if (a->rsn || a->wpa) {
			attron(COLOR_PAIR(COL_SEC_ENC) | A_BOLD);
			mvprintw(row, x, "%-*s", col_widths[5],
			    a->rsn ? "WPA2/RSN" : "WPA");
			attroff(COLOR_PAIR(COL_SEC_ENC) | A_BOLD);
		} else {
			attron(COLOR_PAIR(COL_SEC_OPEN) | A_BOLD);
			mvprintw(row, x, "%-*s", col_widths[5], "OPEN");
			attroff(COLOR_PAIR(COL_SEC_OPEN) | A_BOLD);
		}
		if (sel)
			attron(COLOR_PAIR(COL_SELECTED) | A_BOLD);
		x += col_widths[5] + 1;

		/* BCN */
		mvprintw(row, x, "%-*u", col_widths[6], a->intval);

		if (sel)
			attroff(COLOR_PAIR(COL_SELECTED) | A_BOLD);
	}
}

static void
draw_list_help(int rows, int cols)
{
	attron(COLOR_PAIR(COL_DIM));
	mvhline(rows - 2, 0, ' ', cols);
	mvprintw(rows - 2, 1,
	    " Enter:detail  q:quit  r:rescan  ^/v:nav  0-6:sort  s/S:col  R:rev  ?:help");
	attroff(COLOR_PAIR(COL_DIM));
}

static void
draw_list_view(tui_ctx_t *ctx, int rows, int cols)
{
	/* border under title */
	attron(COLOR_PAIR(COL_BORDER));
	mvhline(1, 0, ACS_HLINE, cols);
	mvhline(rows - (ctx->show_help ? 3 : 2), 0, ACS_HLINE, cols);
	attroff(COLOR_PAIR(COL_BORDER));

	draw_list_header(ctx, cols);
	draw_list_aps(ctx, rows, cols);
	if (ctx->show_help)
		draw_list_help(rows, cols);
}

/* =========================================================================
 * DETAIL PANE  —  helpers
 * ====================================================================== */

/*
 * Print a section header bar (full-width, coloured)
 *   row  : screen row
 *   cols : terminal width
 *   title: section name
 */
static void
detail_section(int row, int cols, const char *title)
{
	attron(COLOR_PAIR(COL_SECTION) | A_BOLD);
	mvhline(row, 0, ' ', cols);
	mvprintw(row, 2, "-- %s ", title);
	int used = 5 + (int)strlen(title);
	if (used < cols - 2)
		mvhline(row, used, '-', cols - used - 1);
	attroff(COLOR_PAIR(COL_SECTION) | A_BOLD);
}

/*
 * Print a labelled key-value line:
 *   col 2   : label  (cyan)
 *   col 22  : value  (white)
 */
static void detail_kv(int row, const char *label, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void
detail_kv(int row, const char *label, const char *fmt, ...)
{
	attron(COLOR_PAIR(COL_LABEL));
	mvprintw(row, 2, "%-20s", label);
	attroff(COLOR_PAIR(COL_LABEL));

	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	attron(COLOR_PAIR(COL_VALUE));
	mvprintw(row, 22, "%s", buf);
	attroff(COLOR_PAIR(COL_VALUE));
}

/* ── RSSI bar (single line, variable width) ──────────────────────────────── */

/*
 * Draw a horizontal RSSI bar at (row, x), width chars wide.
 * rssi range assumed -100 (empty) .. -30 (full).
 */
static void
draw_rssi_bar(int row, int x, int width, int rssi)
{
	if (width <= 2)
		return;
	int inner = width - 2; /* chars inside [ ] */
	int range = 70;	       /* -100 .. -30      */
	int val = rssi + 100;  /* 0..70            */
	if (val < 0)
		val = 0;
	if (val > range)
		val = range;
	int filled = (val * inner) / range;
	if (filled > inner)
		filled = inner;

	int pair = rssi_color(rssi);

	attron(COLOR_PAIR(COL_BORDER));
	mvaddch(row, x, '[');
	mvaddch(row, x + width - 1, ']');
	attroff(COLOR_PAIR(COL_BORDER));

	attron(COLOR_PAIR(pair) | A_BOLD);
	for (int i = 0; i < inner; i++)
		mvaddch(row, x + 1 + i, i < filled ? '#' : '.');
	attroff(COLOR_PAIR(pair) | A_BOLD);
}

/* ── ASCII signal history graph ──────────────────────────────────────────── */

/*
 * Draw a time-series bar graph of RSSI history.
 *
 * Layout (graph_w columns, graph_h rows):
 *
 *  -30 |
 *      |     #
 *      | # # ##
 * -100 +-----------> time
 *
 * Each column = one historical sample (oldest left, newest right).
 * Bar height is proportional to RSSI within [-100, -30].
 * Bars are coloured by signal strength (green/yellow/red).
 * The Y-axis is drawn on the left with dBm labels at top and bottom.
 */
static void
draw_rssi_graph(const wifi_ap_t *a, int top_row, int left_col, int graph_h,
    int graph_w)
{
	if (graph_h < 4 || graph_w < 10)
		return;

	int axis_w = 6; /* characters reserved for Y-axis labels + '|' */
	int plot_w = graph_w - axis_w;
	if (plot_w < 4)
		return;

	int rssi_min = -100;
	int rssi_max = -30;
	int range = rssi_max - rssi_min; /* 70 */

	/* ── Y-axis ── */
	attron(COLOR_PAIR(COL_BORDER));
	for (int r = 0; r < graph_h; r++) {
		int row = top_row + r;
		if (r == 0)
			mvprintw(row, left_col, "%4d|", rssi_max);
		else if (r == graph_h - 1)
			mvprintw(row, left_col, "%4d+", rssi_min);
		else
			mvprintw(row, left_col, "    |");
	}
	/* X-axis label */
	mvprintw(top_row + graph_h - 1, left_col + axis_w, "%.*s", plot_w,
	    "---------> time");
	attroff(COLOR_PAIR(COL_BORDER));

	/* ── bars ── */
	int count = a->hist_count;
	int head = a->hist_head; /* next-write slot = oldest if full */

	/* We show the last min(count, plot_w) samples, newest at right. */
	int show = count < plot_w ? count : plot_w;

	for (int i = 0; i < show; i++) {
		/* sample index: (head - show + i + WIFI_RSSI_HISTORY) %
		 * WIFI_RSSI_HISTORY */
		int sidx = (head - show + i + WIFI_RSSI_HISTORY) %
		    WIFI_RSSI_HISTORY;
		int rssi = (int)a->rssi_hist[sidx];

		int col_x = left_col + axis_w + (plot_w - show) + i;

		/* bar height in rows */
		int val = rssi - rssi_min;
		if (val < 0)
			val = 0;
		if (val > range)
			val = range;
		int bar_h = (val * (graph_h - 1)) / range;
		if (bar_h < 1)
			bar_h = 1;

		int pair = rssi_color(rssi);
		attron(COLOR_PAIR(pair) | A_BOLD);
		for (int r = 0; r < bar_h; r++) {
			int screen_row = top_row + (graph_h - 1) - r - 1;
			/* last row is X-axis label; skip */
			if (screen_row >= top_row &&
			    screen_row < top_row + graph_h - 1)
				mvaddch(screen_row, col_x,
				    r == bar_h - 1 ? '^' : '|');
		}
		attroff(COLOR_PAIR(pair) | A_BOLD);
	}

	/* "no data" placeholder */
	if (show == 0) {
		attron(COLOR_PAIR(COL_DIM));
		mvprintw(top_row + graph_h / 2, left_col + axis_w + 2,
		    "(no history yet)");
		attroff(COLOR_PAIR(COL_DIM));
	}
}

/* ── rates helper ────────────────────────────────────────────────────────── */
static int
detail_print_rates(int row, int col, const wifi_ap_t *a, int max_col)
{
	if (a->nrates == 0) {
		attron(COLOR_PAIR(COL_VALUE));
		mvprintw(row, col, "n/a");
		attroff(COLOR_PAIR(COL_VALUE));
		return row + 1;
	}

	char buf[256];
	buf[0] = '\0';
	int pos = 0;
	for (int i = 0; i < (int)a->nrates && pos < (int)sizeof(buf) - 8; i++) {
		int basic = (a->rates[i] & 0x80) ? 1 : 0; /* basic rate flag */
		int mbps_x2 = a->rates[i] & 0x7F;
		pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%d.%d%s ",
		    basic ? "*" : "", mbps_x2 / 2, (mbps_x2 & 1) ? 5 : 0,
		    basic ? "" : "");
	}

	/* word-wrap into max_col columns starting at col */
	int width = max_col - col;
	if (width < 10)
		width = 10;
	int len = (int)strlen(buf);
	int p = 0;
	while (p < len) {
		attron(COLOR_PAIR(COL_VALUE));
		char line[128];
		int take = len - p < width - 1 ? len - p : width - 1;
		snprintf(line, sizeof(line), "%.*s", take, buf + p);
		mvprintw(row, col, "%s", line);
		attroff(COLOR_PAIR(COL_VALUE));
		p += take;
		row++;
	}
	return row;
}

/* ── capability flags decoded ────────────────────────────────────────────── */
static void
detail_print_caps(int row, const wifi_ap_t *a)
{
	char buf[128];
	int p = 0;
	uint16_t c = a->capinfo;
	if (wifi_cap_ess(c))
		p += snprintf(buf + p, sizeof(buf) - p, "ESS ");
	if (wifi_cap_ibss(c))
		p += snprintf(buf + p, sizeof(buf) - p, "IBSS ");
	if (wifi_cap_privacy(c))
		p += snprintf(buf + p, sizeof(buf) - p, "Privacy ");
	if (wifi_cap_short_preamble(c))
		p += snprintf(buf + p, sizeof(buf) - p, "ShortPreamble ");
	if (wifi_cap_short_slot(c))
		p += snprintf(buf + p, sizeof(buf) - p, "ShortSlot ");
	if (p == 0)
		snprintf(buf, sizeof(buf), "(none)");

	detail_kv(row, "Capabilities", "0x%04x  %s", c, buf);
}

/* =========================================================================
 * DETAIL VIEW — main draw
 * ====================================================================== */

static void
draw_detail_view(const tui_ctx_t *ctx, int rows, int cols)
{
	if (ctx->state->naps == 0)
		return;

	const wifi_ap_t *a = &ctx->state->aps[ctx->selected];
	int row = 2; /* current drawing row (after title) */

	/* ── Identity ── */
	detail_section(row++, cols, "Identity");

	char ssid_disp[IEEE80211_NWID_LEN + 16];
	if (a->ssid[0])
		snprintf(ssid_disp, sizeof(ssid_disp), "%s", a->ssid);
	else
		snprintf(ssid_disp, sizeof(ssid_disp), "<hidden>");
	detail_kv(row++, "SSID", "%s", ssid_disp);
	detail_kv(row++, "BSSID", "%02x:%02x:%02x:%02x:%02x:%02x", a->bssid[0],
	    a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4], a->bssid[5]);
	detail_kv(row++, "Band", "%s", wifi_band_str(a));
	detail_kv(row++, "PHY Mode", "%s", wifi_phy_str(a));
	detail_kv(row++, "Channel", "%u  (%u MHz)", a->chan, a->freq);
	detail_kv(row++, "Extensions", "%s%s%s", a->is_ht ? "HT(n) " : "",
	    a->is_vht ? "VHT(ac) " : "", a->is_he ? "HE(ax) " : "");

	row++; /* blank line */

	/* ── Signal ── */
	detail_section(row++, cols, "Signal");

	/* RSSI value + inline bar */
	{
		attron(COLOR_PAIR(COL_LABEL));
		mvprintw(row, 2, "%-20s", "RSSI");
		attroff(COLOR_PAIR(COL_LABEL));

		attron(COLOR_PAIR(rssi_color((int)a->rssi)) | A_BOLD);
		mvprintw(row, 22, "%4d dBm  ", (int)a->rssi);
		attroff(COLOR_PAIR(rssi_color((int)a->rssi)) | A_BOLD);

		/* bar: from col 32 to col 32+40 */
		int bar_start = 32, bar_width = cols - bar_start - 4;
		if (bar_width < 10)
			bar_width = 10;
		if (bar_start + bar_width < cols)
			draw_rssi_bar(row, bar_start, bar_width, (int)a->rssi);
		row++;
	}

	detail_kv(row++, "Noise Floor", "%4d dBm", (int)a->noise);
	detail_kv(row++, "SNR", "%4d dB%s", a->snr,
	    a->snr >= 25     ? "  (good)" :
		a->snr >= 15 ? "  (fair)" :
			       "  (poor)");

	row++; /* blank line before graph */

	/* ── Signal history graph ── */
	{
		attron(COLOR_PAIR(COL_LABEL) | A_BOLD);
		mvprintw(row, 2, "Signal History");
		attroff(COLOR_PAIR(COL_LABEL) | A_BOLD);
		row++;

		int graph_h = 8;
		int graph_w = cols - 4;
		int remaining = rows - row -
		    4; /* rows left before status bar */
		if (graph_h > remaining - 2)
			graph_h = remaining - 2;
		if (graph_h >= 4)
			draw_rssi_graph(a, row, 2, graph_h, graph_w);
		row += graph_h + 1;
	}

	if (row >= rows - 4)
		goto status_only;

	/* ── 802.11 ── */
	detail_section(row++, cols, "802.11");
	detail_kv(row++, "Beacon Interval", "%u TU  (~%u ms)", a->intval,
	    (unsigned)((float)a->intval * 1.024f + 0.5f));
	detail_kv(row++, "ERP Info", "0x%02x", a->erp);
	detail_print_caps(row++, a);

	attron(COLOR_PAIR(COL_LABEL));
	mvprintw(row, 2, "%-20s", "Supported Rates");
	attroff(COLOR_PAIR(COL_LABEL));
	row = detail_print_rates(row, 22, a, cols - 2);

	if (row >= rows - 4)
		goto status_only;

	/* ── Security ── */
	detail_section(row++, cols, "Security");
	{
		const char *sec = wifi_sec_str(a);
		int pair = (a->rsn || a->wpa) ? COL_SEC_ENC : COL_SEC_OPEN;
		attron(COLOR_PAIR(COL_LABEL));
		mvprintw(row, 2, "%-20s", "Mode");
		attroff(COLOR_PAIR(COL_LABEL));
		attron(COLOR_PAIR(pair) | A_BOLD);
		mvprintw(row, 22, "%s", sec);
		attroff(COLOR_PAIR(pair) | A_BOLD);
		row++;
	}
	detail_kv(row++, "WPA IE", "%s", a->wpa ? "yes" : "no");
	detail_kv(row++, "RSN IE", "%s", a->rsn ? "yes" : "no");
	detail_kv(row++, "Privacy bit", "%s",
	    wifi_cap_privacy(a->capinfo) ? "set" : "clear");
	detail_kv(row++, "Short preamble", "%s",
	    wifi_cap_short_preamble(a->capinfo) ? "yes" : "no");
	detail_kv(row++, "Short slot", "%s",
	    wifi_cap_short_slot(a->capinfo) ? "yes" : "no");

status_only:
	/* Always draw the bottom border */
	attron(COLOR_PAIR(COL_BORDER));
	mvhline(rows - 2, 0, ACS_HLINE, cols);
	attroff(COLOR_PAIR(COL_BORDER));
}

/* =========================================================================
 * PUBLIC: tui_redraw
 * ====================================================================== */

void
tui_redraw(tui_ctx_t *ctx)
{
	int rows, cols;
	getmaxyx(stdscr, rows, cols);
	erase();

	draw_title(ctx, cols);

	/* thin rule under title */
	attron(COLOR_PAIR(COL_BORDER));
	mvhline(1, 0, ACS_HLINE, cols);
	attroff(COLOR_PAIR(COL_BORDER));

	if (ctx->view == TUI_VIEW_LIST)
		draw_list_view(ctx, rows, cols);
	else
		draw_detail_view(ctx, rows, cols);

	draw_status(ctx, rows, cols);
	refresh();
}

/* =========================================================================
 * PUBLIC: tui_handle_key
 * ====================================================================== */

tui_action_t
tui_handle_key(tui_ctx_t *ctx, int ch)
{
	int naps = ctx->state->naps;

	/* ── Detail view key handling ── */
	if (ctx->view == TUI_VIEW_DETAIL) {
		switch (ch) {
		case 27:
		case 'q':
		case 'Q':
		case '\n':
		case KEY_ENTER:
			ctx->view = TUI_VIEW_LIST;
			break;
		case 'r':
			return TUI_ACTION_RESCAN;
		default:
			break;
		}
		return TUI_ACTION_NONE;
	}

	/* ── List view key handling ── */
	switch (ch) {
	case 'q':
	case 'Q':
	case 27:
		return TUI_ACTION_QUIT;

	case '\n':
	case KEY_ENTER:
		if (naps > 0)
			ctx->view = TUI_VIEW_DETAIL;
		break;

	case 'r':
		return TUI_ACTION_RESCAN;

	case 'R':
		ctx->sort_rev = !ctx->sort_rev;
		tui_sort(ctx);
		break;

	case 's':
		ctx->sort_col = (ctx->sort_col + 1) % TUI_NCOLS;
		tui_sort(ctx);
		break;
	case 'S':
		ctx->sort_col = (ctx->sort_col + TUI_NCOLS - 1) % TUI_NCOLS;
		tui_sort(ctx);
		break;

	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
		ctx->sort_col = ch - '0';
		tui_sort(ctx);
		break;

	case KEY_UP:
		if (ctx->selected > 0)
			ctx->selected--;
		break;
	case KEY_DOWN:
		if (ctx->selected < naps - 1)
			ctx->selected++;
		break;
	case KEY_PPAGE:
		ctx->selected -= 10;
		if (ctx->selected < 0)
			ctx->selected = 0;
		break;
	case KEY_NPAGE:
		ctx->selected += 10;
		if (ctx->selected >= naps)
			ctx->selected = naps > 0 ? naps - 1 : 0;
		break;
	case KEY_HOME:
		ctx->selected = 0;
		break;
	case KEY_END:
		ctx->selected = naps > 0 ? naps - 1 : 0;
		break;

	case '?':
		ctx->show_help = !ctx->show_help;
		break;

	default:
		break;
	}

	return TUI_ACTION_NONE;
}
