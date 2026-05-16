#include "fluxipc_internal.h"
#include "fluxipc_shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── 全局状态 ──────────────────────────────────────────── */

static fluxipc_server_t *g_server = NULL;

/* ── 服务端名称推导 ────────────────────────────────────── */

/*
 * 从 /proc/self/exe 获取原始链接二进制名作为逻辑服务端名称。
 * 服务端和客户端通过同一名称找到彼此。
 */
static const char *derive_server_name(const char *prog)
{
    static char buf[64];

    char exe[256];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    const char *base;
    if (len > 0) {
        exe[len] = '\0';
        base = strrchr(exe, '/');
        base = base ? base + 1 : exe;
    } else {
        base = strrchr(prog, '/');
        base = base ? base + 1 : prog;
    }
    strncpy(buf, base, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

/* ── 公开 API ──────────────────────────────────────────── */

void fluxipc_usage(const char *prog)
{
    const char *srv = derive_server_name(prog);
    printf("Server: %s\n", srv);
    fluxipc_registry_dump_usage();
}

int fluxipc_client_init(int argc, char **argv)
{
    const char *srv_name = derive_server_name(argv[0]);

    if (argc < 1) {
        fluxipc_usage(argv[0]);
        return -1;
    }

    const char *cmd_name = argv[0];
    int         cmd_argc = argc - 1;
    char      **cmd_argv = argv + 1;

    /* 建立连接 */
    fluxipc_client_t *cli = fluxipc_client_connect(srv_name);
    if (!cli) {
        fprintf(stderr, "Cannot connect to server '%s'\n", srv_name);
        return -1;
    }

    /* 发起调用 */
    fluxipc_rsp_t rsp;
    void     *data     = NULL;
    size_t    data_len = 0;

    int rc = fluxipc_client_call(cli, cmd_name, cmd_argc, cmd_argv,
                                 &rsp, &data, &data_len);
    if (rc != 0) {
        fprintf(stderr, "call '%s' failed: %d\n", cmd_name, rc);
        fluxipc_client_disconnect(cli);
        return -1;
    }

    if (data) {
        const fluxipc_entry_t *entry = fluxipc_registry_find(cmd_name);
        if (entry && entry->echo)
            entry->echo(data, data_len);

        fluxipc_client_release(cli, &rsp);
    }

    fluxipc_client_disconnect(cli);
    return 0;
}

int fluxipc_interactive_init(int argc, char **argv)
{
    return fluxipc_shell_main(argc, argv);
}

int fluxipc_server_init(const char *prog_name)
{
    const char *srv_name = derive_server_name(prog_name);

    printf("=== FluxIPC Server '%s' ===\n", srv_name);
    fluxipc_registry_dump_usage();
    printf("\n");

    g_server = fluxipc_server_create(srv_name);
    if (!g_server) {
        fprintf(stderr, "Failed to create server '%s'\n", srv_name);
        return -1;
    }
    return 0;
}

int fluxipc_poll(void *data)
{
    (void)data;
    if (g_server) return fluxipc_server_run(g_server);
    return -1;
}

void fluxipc_stop(void)
{
    if (g_server) fluxipc_server_stop(g_server);
}

void fluxipc_destroy(void)
{
    if (g_server) {
        fluxipc_server_destroy(g_server);
        g_server = NULL;
    }
}
