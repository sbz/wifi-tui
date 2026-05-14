# Makefile — wifi-tui TUI (FreeBSD 14.4)
#
# Targets:
#   make            build ./wifi-tui
#   make clean      remove objects and binary
#   make install    install to ${PREFIX}/bin  (default /usr/local/bin)
#   make tags       generate ctags file
#
# Requires BSD make (/usr/bin/make on FreeBSD).

PROG=		wifi-tui
SRCS=		wifi.c tui.c wifi-tui.c

# ncurses is in base on FreeBSD.
# -lm for math (graph rounding).
LDADD=		-lncurses -lm

CFLAGS+=	-O2 -pipe -g
CFLAGS+=	-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+=	-Wshadow -Wcast-align
CFLAGS+=	-I.
CFLAGS+=	-std=c11

# Uncomment to treat warnings as errors:
#CFLAGS+=	-Werror

PREFIX?=	/usr/local
BINDIR=		${PREFIX}/bin

# ── Use bsd.prog.mk when available (standard FreeBSD build infrastructure).
# ── Fall back to explicit rules otherwise.
.if exists(/usr/share/mk/bsd.prog.mk)

.include <bsd.prog.mk>

.else

CC?=	cc
OBJS=	${SRCS:.c=.o}

all: ${PROG}

${PROG}: ${OBJS}
	${CC} ${CFLAGS} -o ${PROG} ${OBJS} ${LDADD}

.c.o:
	${CC} ${CFLAGS} -c -o ${.TARGET} ${.IMPSRC}

# Explicit header deps so objects rebuild when headers change.
wifi.o:		wifi.c wifi.h
tui.o:		tui.c tui.h wifi.h
wifi-tui.o:	wifi-tui.c wifi.h tui.h

install: ${PROG}
	install -d ${DESTDIR}${BINDIR}
	install -m 555 ${PROG} ${DESTDIR}${BINDIR}/${PROG}

tags:
	ctags -R .

clean:
	rm -f ${OBJS} ${PROG} tags

.PHONY: all install tags clean

.endif
