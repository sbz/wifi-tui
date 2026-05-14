# wifi-tui

`FreeBSD` wireless network terminal UI program using `ncurses`. 

It uses `ioctl()` of the Wireless kernel API using `SIOCG80211` to gather
wireless scan results then parse packet and display the information in the
terminal.

It's similar to `ifconfig <dev> scan` but interactive.

Display wireless 802.11x scan results for nearby SSID access points.

- *tui.c* contains the UI primitives using ncurses to draw the window and panes
- *wifi.c* contains the wireless net80211 primitives to parse packet from scan
  result
- *wifi-tui.c* is the main application

## Build

```
make
```

## Run

```
wifi-tui -i <dev> -s <sort> -t <timeout>
```

## FAQ

- Compile out of the box

There is no required dependencies except the libraries from the base systems
(libc, libm and ncurses).

- Supported versions

It is developed and tested under FreeBSD 14.x version, net80211 kernel structures
used in `ioctl()` (_ieee80211req_ and _ieee80211_scan_req_) may changes from a
kernel version to another.
