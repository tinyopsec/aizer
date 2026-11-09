# aizer build.

VERSION = 0.1
PREFIX = /usr/local
BINDIR = ${PREFIX}/bin
MANPREFIX = ${PREFIX}/share/man
MANDIR = ${MANPREFIX}/man1

# per-OS include/lib paths, pick your block:
# Linux (active)
INCS = $(shell pkg-config --cflags freetype2 fontconfig)
LIBS = $(shell pkg-config --libs freetype2 fontconfig) -lwayland-client -lxkbcommon
# FreeBSD / DragonFly:
#INCS = -I/usr/local/include -I/usr/local/include/freetype2
#LIBS = -L/usr/local/lib -lfreetype -lfontconfig -lwayland-client -lxkbcommon
# NetBSD pkgsrc:
#INCS = -I/usr/pkg/include -I/usr/pkg/include/freetype2
#LIBS = -L/usr/pkg/lib -lfreetype -lfontconfig -lwayland-client -lxkbcommon

CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_POSIX_C_SOURCE=200809L -DVERSION=\"${VERSION}\"
CFLAGS = -std=c23 -pedantic -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wshadow -fno-common -O2 -flto ${INCS} ${CPPFLAGS}
LDFLAGS = -flto ${LIBS}
DBGFLAGS = -std=c23 -pedantic -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wshadow -fno-common -g -O0 ${INCS} ${CPPFLAGS}
CC ?= cc
SCANNER = wayland-scanner
#SCANNER = /usr/sbin/wayland-scanner

# layer shell needs xdg-shell symbols (layer surface popup references)
PROTO_SRC = wlr-layer-shell-unstable-v1.xml xdg-shell.xml
GEN_H = wlr-layer-shell-unstable-v1-client-protocol.h xdg-shell-client-protocol.h
GEN_C = wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c
SRC = aizer.c ${GEN_C}
OBJ = aizer.o wlr-layer-shell-unstable-v1-protocol.o xdg-shell-protocol.o

all: aizer

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: aizer.h
aizer.o: ${GEN_H}

aizer: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

debug: aizer_dbg

aizer_dbg: ${SRC} aizer.h ${GEN_H}
	${CC} ${DBGFLAGS} -o $@ ${SRC} ${LIBS}

wlr-layer-shell-unstable-v1-client-protocol.h: wlr-layer-shell-unstable-v1.xml
	${SCANNER} client-header $< $@

wlr-layer-shell-unstable-v1-protocol.c: wlr-layer-shell-unstable-v1.xml
	${SCANNER} private-code $< $@

xdg-shell-client-protocol.h: xdg-shell.xml
	${SCANNER} client-header $< $@

xdg-shell-protocol.c: xdg-shell.xml
	${SCANNER} private-code $< $@

dist: clean
	mkdir aizer-${VERSION}
	cp -R aizer.c aizer.h aizer.1 Makefile LICENSE README.md ${PROTO_SRC} aizer-${VERSION}
	tar -czf aizer-${VERSION}.tar.gz aizer-${VERSION}
	rm -rf aizer-${VERSION}

clean:
	rm -f aizer aizer_dbg ${OBJ} ${GEN_C} ${GEN_H} aizer-${VERSION}.tar.gz

install: aizer
	mkdir -p ${DESTDIR}${BINDIR}
	install -m 755 aizer ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}
	sed s/VERSION/${VERSION}/g < aizer.1 > ${DESTDIR}${MANDIR}/aizer.1
	chmod 644 ${DESTDIR}${MANDIR}/aizer.1

uninstall:
	rm -f ${DESTDIR}${BINDIR}/aizer ${DESTDIR}${MANDIR}/aizer.1

.PHONY: all clean debug dist install uninstall
