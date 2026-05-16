# test/Makefile — single binary, mode selected by command-line flags
CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -g -I../include -D_GNU_SOURCE -Wno-unused-parameter
LDFLAGS  := -L.. -lfluxipc -lreadline -Wl,-rpath='$$ORIGIN/..'

TARGET   := ipc_demo

.PHONY: all clean run-server run-client

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"

clean:
	rm -f $(TARGET)
	rm -f /dev/shm/fluxipc_*
	rm -f /tmp/fluxipc_*.sock

run-server: $(TARGET)
	./$(TARGET) -d

CMD ?= ping hello world
run-client: $(TARGET)
	./$(TARGET) $(CMD)
