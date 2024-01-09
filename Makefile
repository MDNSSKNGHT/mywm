WAYLAND_PROTOCOLS = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER = $(shell pkg-config --variable=wayland_scanner wayland-scanner)
CFLAGS += -DWLR_USE_UNSTABLE
CFLAGS += $(shell pkg-config --cflags wayland-server)
CFLAGS += $(shell pkg-config --cflags wlroots)
CFLAGS += $(shell pkg-config --cflags xkbcommon)
CFLAGS += -I.
CFLAGS += -g3 -O0
LIBS = -lwayland-server -lwlroots -lxkbcommon

all: mywm

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

mywm: xdg-shell-protocol.h mywm.c
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f mywm *.o xdg-shell-protocol.h xdg-shell-protocol.c
