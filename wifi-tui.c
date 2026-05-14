/*
 * wifi-tui.c — entry point for the wifiscan ncurses TUI
 *
 * Build:
 *   make
 *   -- or manually --
 *   cc -O2 -pipe -o wifiscan wifi.c tui.c wifi-tui.c -lncurses -lm
 *
 * Usage:
 *   sudo ./wifiscan wlan0
 *   sudo ./wifiscan -i wlan0 -r 5
 *   sudo ./wifiscan -i wlan0 -r 5 -s 2    # sort by channel
 *
 * Keys (list view):
 *   Enter        open detail pane for selected AP
 *   q / Esc      quit
 *   r            force immediate rescan
 *   Up/Down      move selection
 *   PgUp/PgDn    move 10 rows
 *   Home/End     first / last AP
 *   0-6          sort by column (SSID BSSID CH FREQ RSSI SEC BCN)
 *   s / S        cycle sort column forward / backward
 *   R            toggle reverse sort order
 *   ?            toggle help bar
 *
 * Keys (detail pane):
 *   Esc / q / Enter   return to list
 *   r                 force rescan (updates signal history)
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tui.h"
#include "wifi.h"

/* ── SIGWINCH ────────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_resize = 0;
static void
handle_sigwinch(int sig)
{
	(void)sig;
	g_resize = 1;
}

/* ── usage ───────────────────────────────────────────────────────────────── */
static void
usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [-i] <ifname> [-r <seconds>] [-s <col>]\n"
	    "\n"
	    "  -i <iface>   wireless interface  (e.g. wlan0)\n"
	    "  -r <sec>     refresh interval    (default: %d s)\n"
	    "  -s <0-6>     initial sort column (default: 4 = RSSI)\n"
	    "\n"
	    "Columns: 0=SSID  1=BSSID  2=CH  3=FREQ  4=RSSI  5=SEC  6=BCN\n"
	    "\n"
	    "Must be run as root (requires net80211 ioctl privileges).\n",
	    prog, WIFI_DEFAULT_REFRESH);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int
main(int argc, char **argv)
{
	char ifname[IFNAMSIZ] = "";
	int refresh = WIFI_DEFAULT_REFRESH;
	int initial_sort = TUI_SORT_RSSI;
	int opt;

	while ((opt = getopt(argc, argv, "i:r:s:h")) != -1) {
		switch (opt) {
		case 'i':
			strlcpy(ifname, optarg, sizeof(ifname));
			break;
		case 'r':
			refresh = atoi(optarg);
			break;
		case 's':
			initial_sort = atoi(optarg) % TUI_NCOLS;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	/* Accept bare ifname as first positional argument */
	if (ifname[0] == '\0' && optind < argc)
		strlcpy(ifname, argv[optind], sizeof(ifname));

	if (ifname[0] == '\0') {
		usage(argv[0]);
		return 2;
	}
	if (refresh < 1)
		refresh = 1;

	int sock = wifi_open_socket();
	if (sock < 0)
		return 1;

	wifi_state_t state;
	memset(&state, 0, sizeof(state));

	tui_ctx_t ctx;
	tui_init(&ctx, &state, ifname, refresh);
	ctx.sort_col = initial_sort;

	signal(SIGWINCH, handle_sigwinch);

	/* Initial scan */
	state.last_ok = (wifi_scan(sock, ifname, &state) == 0);
	tui_sort(&ctx);
	tui_redraw(&ctx);

	/* ── event loop ── */
	int running = 1;
	while (running) {
		if (g_resize) {
			g_resize = 0;
			endwin();
			refresh();
			clear();
		}

		/* Auto-refresh */
		if (time(NULL) - state.last_scan >= (time_t)refresh) {
			state.last_ok = (wifi_scan(sock, ifname, &state) == 0);
			tui_sort(&ctx);
		}

		tui_redraw(&ctx);

		int ch = getch();
		if (ch == ERR)
			continue;

		tui_action_t action = tui_handle_key(&ctx, ch);
		switch (action) {
		case TUI_ACTION_QUIT:
			running = 0;
			break;
		case TUI_ACTION_RESCAN:
			state.last_scan =
			    0; /* expire timer → rescan on next loop */
			break;
		case TUI_ACTION_DETAIL:
		case TUI_ACTION_NONE:
		default:
			break;
		}
	}

	tui_exit();
	close(sock);
	return 0;
}
