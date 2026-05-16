/*
 * fluxipc_shell.c
 * 基于 GNU Readline 的 FluxIPC 交互式 shell
 */

#include "fluxipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>

#include <readline/readline.h>
#include <readline/history.h>

/* ── 注册表辅助 ────────────────────────────────────────── */

extern const fluxipc_entry_t *
fluxipc_registry_find(const char *name);

/* ── Shell 状态 ────────────────────────────────────────── */

static volatile sig_atomic_t g_shell_running = 1;

/* ── 信号处理 ──────────────────────────────────────────── */

static void shell_on_signal(int sig)
{
    (void)sig;

    g_shell_running = 0;

    rl_replace_line("", 0);
    rl_done = 1;
}

/* ── 工具函数 ──────────────────────────────────────────── */

static char *trim_left(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

static void trim_right(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static char *trim(char *s)
{
    s = trim_left(s);
    trim_right(s);
    return s;
}

/* ── 分词器 ────────────────────────────────────────────── */

#define SHELL_MAX_ARGS 64

static int shell_parse(char *line, char **argv)
{
    int argc = 0;

    char *tok = strtok(line, " \t");

    while (tok && argc < SHELL_MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }

    argv[argc] = NULL;

    return argc;
}

/* ── IPC 命令执行 ──────────────────────────────────────── */

/*
 * 复用当前可执行文件直接执行 IPC 命令（客户端模式为默认模式）。
 *
 *   myapp get_stats
 */
static int shell_execute_ipc(int argc, char **argv)
{
    if (argc <= 0)
        return -1;

    const char *cmd_name = argv[0];

    const fluxipc_entry_t *entry =
        fluxipc_registry_find(cmd_name);

    if (!entry)
        return -1;

    char cmdline[4096];

    size_t off = 0;

    off += snprintf(cmdline + off,
                    sizeof(cmdline) - off,
                    "%s",
                    program_invocation_name);

    for (int i = 0; i < argc; i++) {
        off += snprintf(cmdline + off,
                        sizeof(cmdline) - off,
                        " %s",
                        argv[i]);
    }

    return system(cmdline);
}

/* ── 内建命令 ──────────────────────────────────────────── */

static void shell_help(void)
{
    printf("\n");

    printf("Builtins:\n");
    printf("  help                 Show help\n");
    printf("  history              Show history\n");
    printf("  clear                Clear screen\n");
    printf("  watch CMD            Run command repeatedly\n");
    printf("  quit/exit            Exit shell\n");
    printf("  !cmd                 Run system command\n");

    printf("\n");

    printf("IPC Commands:\n");

    for (fluxipc_entry_t *e = __start_fluxipc_registry;
         e < __stop_fluxipc_registry;
         e++)
    {
        printf("  %-20s %s\n",
               e->name,
               e->usage ? e->usage : "");
    }

    printf("\n");
}

static void shell_history(void)
{
    HIST_ENTRY **hist = history_list();

    if (!hist)
        return;

    for (int i = 0; hist[i]; i++) {
        printf("%4d  %s\n", i + history_base, hist[i]->line);
    }
}

static int shell_builtin(int argc, char **argv)
{
    if (argc <= 0)
        return 0;

    if (strcmp(argv[0], "help") == 0) {
        shell_help();
        return 1;
    }

    if (strcmp(argv[0], "history") == 0) {
        shell_history();
        return 1;
    }

    if (strcmp(argv[0], "clear") == 0) {
        system("clear");
        return 1;
    }

    if (strcmp(argv[0], "exit") == 0 ||
        strcmp(argv[0], "quit") == 0)
    {
        g_shell_running = 0;
        return 1;
    }

    return 0;
}

/* ── watch 实现 ────────────────────────────────────────── */

static void shell_watch_command(const char *cmdline)
{
    printf("watch mode: press 'q' + ENTER to stop\n");

    while (1)
    {
        system("clear");

        shell_execute_ipc(0, NULL);

        printf("\n");
        printf("watch> %s\n", cmdline);

        fd_set rfds;

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int rc = select(STDIN_FILENO + 1,
                        &rfds,
                        NULL,
                        NULL,
                        &tv);

        if (rc > 0 && FD_ISSET(STDIN_FILENO, &rfds))
        {
            char buf[32];

            if (fgets(buf, sizeof(buf), stdin))
            {
                if (buf[0] == 'q')
                    break;
            }
        }
    }
}

/* ── 命令分发 ──────────────────────────────────────────── */

static void shell_dispatch(char *line)
{
    line = trim(line);

    if (*line == '\0')
        return;

    /*
     * 系统命令：!ls
     */

    if (line[0] == '!')
    {
        system(line + 1);
        return;
    }

    /*
     * watch
     */

    if (strncmp(line, "watch ", 6) == 0)
    {
        shell_watch_command(line + 6);
        return;
    }

    char *argv[SHELL_MAX_ARGS];

    char tmp[4096];

    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    int argc = shell_parse(tmp, argv);

    if (argc <= 0)
        return;

    /*
     * 内建命令
     */

    if (shell_builtin(argc, argv))
        return;

    /*
     * IPC 命令
     */

    if (shell_execute_ipc(argc, argv) == 0)
        return;

    printf("Unknown command: %s\n", argv[0]);
}

/* ── Readline 补全 ─────────────────────────────────────── */

static const char *g_builtin_cmds[] = {
    "help",
    "history",
    "clear",
    "watch",
    "exit",
    "quit",
    NULL
};

static char *shell_command_generator(const char *text, int state)
{
    static int list_index;
    static int registry_index;
    static size_t len;

    if (!state)
    {
        list_index     = 0;
        registry_index = 0;
        len            = strlen(text);
    }

    /*
     * 内建命令
     */

    while (g_builtin_cmds[list_index])
    {
        const char *name = g_builtin_cmds[list_index++];

        if (strncmp(name, text, len) == 0)
            return strdup(name);
    }

    /*
     * IPC 注册表
     */

    while ((__start_fluxipc_registry + registry_index)
           < __stop_fluxipc_registry)
    {
        fluxipc_entry_t *e =
            &__start_fluxipc_registry[registry_index++];

        if (strncmp(e->name, text, len) == 0)
            return strdup(e->name);
    }

    return NULL;
}

static char **shell_completion(const char *text,
                               int start,
                               int end)
{
    (void)end;

    /*
     * 第一个 token：命令补全
     */

    if (start == 0)
    {
        return rl_completion_matches(
            text,
            shell_command_generator);
    }

    /*
     * 否则回退到文件名补全
     */

    return NULL;
}

/* ── 主入口 ────────────────────────────────────────────── */

int fluxipc_shell_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    signal(SIGINT,  shell_on_signal);
    signal(SIGTERM, shell_on_signal);

    using_history();

    rl_attempted_completion_function =
        shell_completion;

    printf("=== FluxIPC Interactive Shell ===\n");
    printf("Type 'help' for commands.\n\n");

    while (g_shell_running)
    {
        char *line = readline("fluxipc> ");

        if (!line)
            break;

        char *s = trim(line);

        if (*s)
            add_history(s);

        shell_dispatch(s);

        free(line);
    }

    printf("\nbye.\n");

    return 0;
}
