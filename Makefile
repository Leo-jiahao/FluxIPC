# FluxIPC Makefile — libfluxipc.so.1.0
CC       ?= gcc
CFLAGS   += -Wall -Wextra -O2 -g \
            -Iinclude \
            -D_GNU_SOURCE \
            -Wno-unused-parameter
LDLIBS  += -lrt -lpthread -lreadline 

PREFIX     ?= /usr
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include/fluxipc

# ── Sources ──────────────────────────────────────────

LIB_SRCS := src/fluxipc_slot.c \
            src/fluxipc_shm.c  \
            src/fluxipc_registry.c \
            src/fluxipc_server.c \
            src/fluxipc_client.c \
            src/fluxipc.c \
            src/fluxipc_shell.c

LIB_PIC_OBJS := $(LIB_SRCS:.c=.pic.o)

TARGET_LIB := libfluxipc.so.1.0
SONAME     := libfluxipc.so.1

.PHONY: all clean install uninstall lib test

all: lib

lib: $(TARGET_LIB)

# ── Shared library ───────────────────────────────────

$(TARGET_LIB): $(LIB_PIC_OBJS)
	$(CC) $(LDFLAGS) -shared \
	    -Wl,-soname,$(SONAME) \
	    -o $@ \
	    $^ $(LDLIBS)

	ln -sf $(TARGET_LIB) libfluxipc.so.1
	ln -sf $(TARGET_LIB) libfluxipc.so

src/%.pic.o: src/%.c
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

# ── Clean ────────────────────────────────────────────

clean:
	rm -f $(LIB_PIC_OBJS)
	rm -f $(TARGET_LIB)
	rm -f $(SONAME) libfluxipc.so
	rm -f /dev/shm/fluxipc_*
	rm -f /tmp/fluxipc_*.sock
	$(MAKE) -C test clean

# ── Install / Uninstall ──────────────────────────────

install: lib
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -m 755 $(TARGET_LIB) $(DESTDIR)$(LIBDIR)/
	ln -sf $(TARGET_LIB) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME)       $(DESTDIR)$(LIBDIR)/libfluxipc.so
	install -m 644 include/fluxipc.h $(DESTDIR)$(INCLUDEDIR)/
	ldconfig $(DESTDIR)$(LIBDIR) 2>/dev/null || true
	@echo "FluxIPC installed to $(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(TARGET_LIB)
	rm -f $(DESTDIR)$(LIBDIR)/$(SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/libfluxipc.so
	rm -rf $(DESTDIR)$(INCLUDEDIR)
	ldconfig $(DESTDIR)$(LIBDIR) 2>/dev/null || true
	@echo "FluxIPC uninstalled"

# ── Test targets ─────────────────────────────────────

test: lib
	$(MAKE) -C test all
	@echo "=== Starting server in background ==="
	./test/ipc_demo &
	@SERVER_PID=$$!; \
	sleep 0.3; \
	echo "=== Single calls ==="; \
	./test/ipc_demo_cli ping hello world; \
	./test/ipc_demo_cli get_stats cpu; \
	./test/ipc_demo_cli get_map north; \
	echo "=== Stopping server ==="; \
	kill $$SERVER_PID 2>/dev/null; \
	wait $$SERVER_PID 2>/dev/null; \
	echo "=== Test complete ==="
