# test/Makefile — single binary + symlink for client role
CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -g -I../include -D_GNU_SOURCE -Wno-unused-parameter
LDFLAGS  := -L.. -lfluxipc -lreadline -Wl,-rpath='$$ORIGIN/..'

TARGET   := ipc_demo
CLI_LINK := ipc_demo_cli

.PHONY: all clean run-server run-client

all: $(TARGET) $(CLI_LINK)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"

$(CLI_LINK): $(TARGET)
	ln -sf $(TARGET) $@
	@echo "Linked: $@ -> $(TARGET)"

clean:
	rm -f $(TARGET) $(CLI_LINK)
	rm -f /dev/shm/fluxipc_*
	rm -f /tmp/fluxipc_*.sock

run-server: $(TARGET)
	./$(TARGET)

CMD ?= ping hello world
run-client: $(TARGET) $(CLI_LINK)
	./$(CLI_LINK) $(CMD)
