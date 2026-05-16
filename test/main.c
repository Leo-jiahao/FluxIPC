/*
 * test/main.c — FluxIPC 测试应用
 *
 * 单个二进制，通过命令行参数选择角色：
 *   ./ipc_demo  ping hello          → 客户端（默认）
 *   ./ipc_demo  -c, --fic           → 交互式 shell
 *   ./ipc_demo  -d, --fid           → 服务端
 */

#include "fluxipc.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>

/* ── 服务端优雅关闭的信号处理 ──────────────────────────── */

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    fluxipc_stop();
    g_running = 0;
}

/* ── 客户端输出回调 ────────────────────────────────────── */

static void echo_json(const void *data, size_t len)
{
    printf("[echo] JSON (%zu bytes): %.*s\n", len, (int)len, (const char *)data);
}

static void echo_map(const void *data, size_t len)
{
    printf("[echo] Map:\n%.*s", (int)len, (const char *)data);
    if (len > 0 && ((const char *)data)[len - 1] != '\n')
        printf("\n");
}

/* ── 服务端处理函数 ────────────────────────────────────── */

static int cmd_ping(int argc, char **argv,
                    void *slot_data, size_t slot_sz)
{
    (void)slot_data; (void)slot_sz;
    printf("[handler] ping called with %d args\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);
    return 0;
}
REGISTER_FLUXIPC("ping",
                 "ping [message...]  — 控制面往返测试",
                 0, 0,
                 cmd_ping,
                 NULL);

static int cmd_get_stats(int argc, char **argv,
                         void *slot_data, size_t slot_sz)
{
    const char *filter = (argc > 0) ? argv[0] : "all";
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    int n = snprintf((char *)slot_data, slot_sz,
        "{\"filter\":\"%s\","
        " \"cpu_pct\":%.1f,"
        " \"mem_mb\":%d,"
        " \"uptime_s\":%ld,"
        " \"pid\":%d}",
        filter,
        (float)(ts.tv_nsec % 1000) / 10.0f,
        256 + (int)(ts.tv_nsec % 512),
        ts.tv_sec,
        getpid());

    printf("[handler] get_stats filter=%s  wrote %d bytes\n", filter, n);
    return 0;
}
REGISTER_FLUXIPC("get_stats",
                 "get_stats [filter]  — 返回 JSON 格式系统状态",
                 1, 4096,
                 cmd_get_stats,
                 echo_json);

static int cmd_get_map(int argc, char **argv,
                       void *slot_data, size_t slot_sz)
{
    const char *region = (argc > 0) ? argv[0] : "default";
    int n = snprintf((char *)slot_data, slot_sz,
        "MAP[region=%s]\n"
        "+---------+\n"
        "|  .  .  .|\n"
        "|  . [X] .|\n"
        "|  .  .  .|\n"
        "+---------+\n",
        region);
    printf("[handler] get_map region=%s  wrote %d bytes\n", region, n);
    return 0;
}
REGISTER_FLUXIPC("get_map",
                 "get_map [region]  — 返回 ASCII 区域地图",
                 2, 8192,
                 cmd_get_map,
                 echo_map);

/* ── main ──────────────────────────────────────────────── */

typedef enum { MODE_CLIENT, MODE_SERVER, MODE_INTERACTIVE } app_mode_t;

static void print_app_usage(const char *prog)
{
    printf("FluxIPC — Inter-Process Communication Framework\n"
           "\n"
           "Usage modes:\n"
           "  %s  <command> [args...]    执行 IPC 命令 (客户端)\n"
           "  %s  -c, --fic              交互式 shell (客户端)\n"
           "  %s  -d, --fid              启动 IPC 服务 (守护进程)\n"
           "  %s  -h, --help             显示此帮助\n"
           "\n",
           prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    app_mode_t mode = MODE_CLIENT;

    static struct option long_opts[] = {
        {"fic",  no_argument, 0, 'c'},
        {"fid",  no_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "cdh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c': mode = MODE_INTERACTIVE; break;
        case 'd': mode = MODE_SERVER;      break;
        case 'h': print_app_usage(argv[0]); fluxipc_usage(argv[0]); return 0;
        default:  print_app_usage(argv[0]); fluxipc_usage(argv[0]); return 1;
        }
    }

    switch (mode) {
    case MODE_SERVER: {
        if (fluxipc_server_init(argv[0]) != 0)
            return 1;

        signal(SIGINT,  on_signal);
        signal(SIGTERM, on_signal);

        while (g_running) {
            if (fluxipc_poll(NULL) != 0)
                break;
        }

        fluxipc_destroy();
        return 0;
    }
    case MODE_INTERACTIVE:
        return fluxipc_interactive_init(argc, argv);

    case MODE_CLIENT:
    default:
        if (optind >= argc) {
            print_app_usage(argv[0]);
            fluxipc_usage(argv[0]);
            return 1;
        }
        return fluxipc_client_init(argc - optind, argv + optind);
    }
}
