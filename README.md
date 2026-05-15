# FluxIPC

High-performance shared-memory IPC framework for Linux, with a client-server model, compile-time command registration, and an interactive shell.

## Architecture

FluxIPC uses a dual-plane design:

- **Control plane** — Unix domain socket (`SOCK_STREAM`) for lightweight command dispatch and metadata exchange.
- **Data plane** — POSIX shared memory (`/dev/shm/fluxipc_<name>`) organized as a slot pool with `PTHREAD_PROCESS_SHARED` synchronization, avoiding bulk data copying through the socket.

Commands are registered at compile time via the `REGISTER_FLUXIPC()` macro, which places entries in a dedicated ELF section (`.fluxipc_registry`). The linker-provided boundary symbols `__start_fluxipc_registry` and `__stop_fluxipc_registry` are used to enumerate commands at runtime — no manual registration or boilerplate.

Role detection is automatic based on the binary filename:
- Name contains `cli`, `cmd`, or `client` → **client** mode
- `argv[1] == "--fic <command>"` → force client mode
- `argv[1] == "--flc"` → **interactive shell** mode
- Otherwise → **server** mode

## Slot State Machine

Each shared memory slot transitions through a strict lifecycle:

```
FREE → WRITING → READY → CONSUMING → FREE
```

- **FREE** — slot is available for allocation.
- **WRITING** — server has claimed the slot and is writing response data.
- **READY** — server has finished writing; client may consume.
- **CONSUMING** — client has acquired the slot for reading.
- **FREE** — client releases the slot back to the pool.

Generation numbers (`slot_gen`) protect against stale references. If a client attempts to acquire a slot whose generation has changed, the call returns `-ESTALE`.

## Dependencies

| Dependency | Purpose |
|---|---|
| Linux 2.6+ | epoll, POSIX shared memory, Unix sockets |
| librt | `shm_open`, `shm_unlink`, `mmap` |
| libpthread | `PTHREAD_PROCESS_SHARED` mutexes and condition variables |
| libreadline | Interactive shell (only needed for `--flc` mode) |
| GCC (or C99 compiler) | GNU `__attribute__` for ELF section placement |

## Build & Install

```bash
make              # build libfluxipc.so.1.0
make test         # build library + test binary, run demo
sudo make install # install to /usr/local (override with PREFIX=)
sudo make uninstall
```

Install paths (defaults):

| Variable | Default |
|---|---|
| `PREFIX` | `/usr/local` |
| `LIBDIR` | `$(PREFIX)/lib` |
| `INCLUDEDIR` | `$(PREFIX)/include/fluxipc` |

## Quick Start

### 1. Register commands

```c
#include "fluxipc.h"

// Control-plane-only command (no shared memory)
static int cmd_ping(int argc, char **argv, void *slot_data, size_t slot_sz)
{
    (void)slot_data; (void)slot_sz;
    printf("ping: %d args\n", argc);
    return 0;
}
REGISTER_FLUXIPC("ping", "ping [msg...]  — roundtrip test", 0, 0, cmd_ping, NULL);

// Data-plane command (uses shared memory for response)
static void echo_json(const void *data, size_t len) {
    printf("%.*s\n", (int)len, (const char *)data);
}

static int cmd_get_stats(int argc, char **argv, void *slot_data, size_t slot_sz)
{
    int n = snprintf(slot_data, slot_sz,
        "{\"cpu_pct\": 42.3, \"mem_mb\": 512}");
    return 0;
}
REGISTER_FLUXIPC("get_stats", "get_stats  — JSON stats",
                 1, 4096, cmd_get_stats, echo_json);
```

### 2. Run the server

```c
int main(int argc, char **argv)
{
    if (fluxipc_init(argc, argv) != 0)
        return 1;

    // fluxipc_init returns only in server mode
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (g_running)
        fluxipc_poll(NULL);

    fluxipc_destroy();
    return 0;
}
```

### 3. Call from a client

```bash
ln -s my_app my_app_cli
./my_app_cli ping hello world
./my_app_cli get_stats
./my_app --flc          # interactive shell
./my_app --fic ping hi  # force client mode
```

## API Reference

### Public API (`fluxipc.h`)

```c
int  fluxipc_init(int argc, char **argv);  // detect role and initialize
int  fluxipc_poll(void *data);              // server event loop step (epoll_wait)
void fluxipc_stop(void);                    // signal server to exit
void fluxipc_destroy(void);                 // cleanup sockets, shared memory
```

### Registration Macro

```c
REGISTER_FLUXIPC(cmd_name, usage_str, shm_obj_id, slot_data_sz, handler, echo_fn)
```

| Parameter | Description |
|---|---|
| `cmd_name` | Command string the client sends (e.g. `"get_stats"`) |
| `usage_str` | Help text shown by `--flc` shell |
| `shm_obj_id` | Non-zero unique ID for data-plane commands; `0` for control-plane only |
| `slot_data_sz` | Bytes per slot (ignored if `shm_obj_id == 0`) |
| `handler` | `int handler(int argc, char **argv, void *slot_data, size_t slot_sz)` — server-side, return `0` on success or negative errno |
| `echo_fn` | `void echo(const void *data, size_t len)` — client-side output callback, or `NULL` |

### Handler Callbacks

```c
typedef int (*fluxipc_handler_fn)(int argc, char **argv,
                                  void *slot_data, size_t slot_sz);

typedef void (*fluxipc_echo_fn)(const void *data, size_t len);
```

## Interactive Shell

Launch with `./my_app --flc`. Features:

- **Tab completion** for both built-in and registered IPC commands.
- **`help`** — list all commands with usage strings.
- **`history`** — show command history.
- **`watch <cmd>`** — repeat a command every second; press `w` to increase interval, `s` to decrease, `q` to quit.
- **`exit` / `quit`** — exit the shell.
- **`!<shell_cmd>`** — run a system command (e.g. `!ls -la`).

## Wire Protocol

All multi-byte fields are host-endian (single-machine IPC).

**Request** (client → server):

```
┌──────────┬──────────┬──────────┬──────────┬──────────────┐
│  cmd_id  │   seq    │   argc   │ data_len │ data (var)   │
│ (32-bit) │ (32-bit) │ (32-bit) │ (32-bit) │ NUL-separated│
└──────────┴──────────┴──────────┴──────────┴──────────────┘
```

**Response** (server → client):

```
┌──────────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
│  cmd_id  │   seq    │  status  │shm_obj_id│ slot_idx │ slot_gen │data_size │
│ (32-bit) │ (32-bit) │ (32-bit) │ (32-bit) │ (32-bit) │ (32-bit) │ (32-bit) │
└──────────┴──────────┴──────────┴──────────┴──────────┴──────────┴──────────┘
```

- `shm_obj_id == 0` → control-plane only, no shared memory data.
- `status == 0` → success; negative value → errno.

## Shared Memory Layout

```
┌─────────────────────┐
│  global header      │  magic, version, obj_count, total_size
├─────────────────────┤
│  obj descriptors    │  obj_desc[0..N-1]
├─────────────────────┤
│  slot pool 0        │  pool_hdr + slot_hdr[0..M-1] + data[0..M-1]
├─────────────────────┤
│  slot pool 1        │
├─────────────────────┤
│  ...                │
└─────────────────────┘
```

Slots are 64-byte-aligned to match cache line boundaries. Each slot has its own `pthread_mutex_t` and `pthread_cond_t` for process-shared synchronization.

## Test Application

```bash
make test
```

This builds `test/ipc_demo`, creates a symlink `ipc_demo_cli → ipc_demo`, starts the server, runs three test commands (`ping`, `get_stats`, `get_map`), and stops the server.

The test application registers three commands:

| Command | Type | Description |
|---|---|---|
| `ping` | Control-plane | Echoes arguments back |
| `get_stats` | Data-plane (4 KB slot) | Returns a JSON status blob |
| `get_map` | Data-plane (8 KB slot) | Returns an ASCII grid map |

## Yocto / OpenEmbedded

A BitBake recipe is provided at [scripts/fluxipc.bb](scripts/fluxipc.bb). It splits output into:

- `fluxipc` — runtime shared library
- `fluxipc-dev` — headers and `.so` symlink
- `fluxipc-dbg` — debug symbols

```
RDEPENDS:${PN} = "readline"
```

## License

MIT
