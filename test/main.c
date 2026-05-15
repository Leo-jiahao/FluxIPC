/*
 * test/main.c — FluxIPC 测试应用
 *
 * 单个二进制，通过 symlink 或 argv 在运行时选择角色：
 *   ./ipc_demo                        → 服务端（按名称自动识别）
 *   ./ipc_demo_cli <cmd> [args...]    → 客户端（名称含 "cli"）
 */

#include "fluxipc.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

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

int main(int argc, char **argv)
{
    if (fluxipc_init(argc, argv) != 0)
        return 1;

    /* fluxipc_init() 在客户端/交互角色中不会返回，
     * 到达此处即说明我们是服务端。 */

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    while (g_running) {
        if (fluxipc_poll(NULL) != 0)
            break;
    }

    fluxipc_destroy();
    return 0;
}
