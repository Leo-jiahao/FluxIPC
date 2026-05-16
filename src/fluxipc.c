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
 * 从二进制文件名中去除已知角色后缀，得到逻辑服务端名称。
 * 服务端和客户端通过同一名称找到彼此。
 *
 *   test_server / test_client  →  "test"
 *   myapp       / myapp_cli    →  "myapp"
 */
static const char *derive_server_name(const char *prog)
{
    static char buf[64];

    /* 使用 /proc/self/exe 获取实际二进制名，而非 argv[0] */
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

    /* 后缀按长度降序排列，确保 "_client" 优先于 "client" 匹配 */
    static const char *suffixes[] = {
        "_server", "-server", "server",
        "_client", "-client", "client",
        "_cli",    "-cli",    "cli",
        "_cmd",    "-cmd",    "cmd",
    };
    size_t blen = strlen(buf);
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t slen = strlen(suffixes[i]);
        if (blen > slen && strcmp(buf + blen - slen, suffixes[i]) == 0) {
            blen -= slen;
            buf[blen] = '\0';
            /* 去掉可能残留的分隔符 */
            if (blen > 0 && (buf[blen - 1] == '_' || buf[blen - 1] == '-'))
                buf[--blen] = '\0';
            break;
        }
    }
    return buf;
}

/* ── 角色检测 ──────────────────────────────────────────── */

typedef enum { ROLE_SERVER, ROLE_CLIENT, ROLE_INTERACTIVE } role_t;

static role_t detect_role(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--fic") == 0)
        return ROLE_INTERACTIVE;

    if (argc > 2 && strcmp(argv[1], "--fic") == 0)
        return ROLE_CLIENT;

    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];

    if (strstr(base, "client") || strstr(base, "cli") || strstr(base, "cmd"))
        return ROLE_CLIENT;

    return ROLE_SERVER;
}

/* ── 客户端模式 ────────────────────────────────────────── */

static void fluxipc_usage(const char *prog)
{
    const char *srv = derive_server_name(prog);
    fprintf(stderr,
        "Usage:\n"
        "  %s <command> [args...]          调用 IPC 命令\n"
        "  %s --fic <command> [args...]    调用 IPC 命令（显式指定）\n"
        "  %s --fic                        交互式 shell\n"
        "  %s --help, -h                   显示帮助\n"
        "\n"
        "可用命令 (server: %s):\n",
        prog, prog, prog, prog, srv);
    fluxipc_registry_dump_usage();
}

static int run_client(int argc, char **argv)
{
    const char *srv_name = derive_server_name(argv[0]);

    /* 解析命令名和参数 */
    const char *cmd_name;
    int         cmd_argc;
    char      **cmd_argv;

    if (argc > 1 && strcmp(argv[1], "--fic") == 0) {
        if (argc < 3) {
            fluxipc_usage(argv[0]);
            exit(1);
        }
        cmd_name = argv[2];
        cmd_argc = argc - 3;
        cmd_argv = argv + 3;
    } else {
        if (argc < 2) {
            fluxipc_usage(argv[0]);
            exit(1);
        }
        cmd_name = argv[1];
        cmd_argc = argc - 2;
        cmd_argv = argv + 2;
    }

    /* 建立连接 */
    fluxipc_client_t *cli = fluxipc_client_connect(srv_name);
    if (!cli) {
        fprintf(stderr, "Cannot connect to server '%s'\n", srv_name);
        exit(1);
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
        exit(1);
    }

    if (data) {
        /* 查找命令对应的输出回调 */
        const fluxipc_entry_t *entry = fluxipc_registry_find(cmd_name);
        if (entry && entry->echo)
            entry->echo(data, data_len);

        fluxipc_client_release(cli, &rsp);
    }

    fluxipc_client_disconnect(cli);
    exit(0);
}

/* ── 服务端模式 ────────────────────────────────────────── */

static int create_server(int argc, char **argv)
{
    (void)argc;
    const char *srv_name = derive_server_name(argv[0]);

    printf("=== FluxIPC Server '%s' ===\n", srv_name);
    fluxipc_registry_dump_usage();
    printf("\n");

    g_server = fluxipc_server_create(srv_name);
    if (!g_server) {
        fprintf(stderr, "Failed to create server '%s'\n", srv_name);
        return 1;
    }
    return 0;
}

/* ── 交互模式 ──────────────────────────────────────────── */

static int run_interactive(int argc, char **argv)
{
    fluxipc_shell_main(argc, argv);
    exit(0);
}

/* ── 公开入口 ──────────────────────────────────────────── */

int fluxipc_init(int argc, char **argv)
{
    /* --help / -h */
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        fluxipc_usage(argv[0]);
        exit(0);
    }

    role_t role = detect_role(argc, argv);

    switch (role) {
    case ROLE_CLIENT:      return run_client(argc, argv);
    case ROLE_INTERACTIVE: return run_interactive(argc, argv);
    case ROLE_SERVER:
    default:               return create_server(argc, argv);
    }
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
