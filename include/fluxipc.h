#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * FluxIPC — 统一 IPC 库
 *
 * 只需包含此头文件即可使用。根据使用场景调用不同初始化接口：
 *
 *   fluxipc_client_init(argc, argv)       客户端模式，执行一条 IPC 命令
 *   fluxipc_interactive_init(argc, argv)  交互式 shell 模式
 *   fluxipc_server_init(prog_name)        服务端模式
 *
 * 命令通过 REGISTER_FLUXIPC() 在编译期注册。
 */

/* ── 回调类型 ───────────────────────────────────────────── */

typedef void (*fluxipc_echo_fn)(const void *data, size_t len);


typedef int (*fluxipc_handler_fn)(int argc, char **argv,
                                  void *slot_data, size_t slot_sz);

/* ── 注册条目 ───────────────────────────────────────────── */

typedef struct {
    const char         *name;
    const char         *usage;
    uint32_t            shm_obj_id;    /* 框架启动时自动填充，0 = 无数据平面 */
    size_t              slot_data_sz;  /* > 0 则自动分配数据平面 */
    fluxipc_handler_fn  handler;
    fluxipc_echo_fn     echo;          /* 客户端输出回调，可为 NULL */
} fluxipc_entry_t;

/* 链接器提供的段边界 */
extern fluxipc_entry_t __start_fluxipc_registry[];
extern fluxipc_entry_t __stop_fluxipc_registry[];

#define FLUXIPC_SECTION \
    __attribute__((section("fluxipc_registry"), used, aligned(sizeof(void *))))

/*
 * REGISTER_FLUXIPC(cmd_name, usage_str, slot_data_sz, handler, echo_fn)
 *
 *   cmd_name     - 客户端发送的命令字符串（如 "get_stats"）
 *   usage_str    - 帮助文本
 *   slot_data_sz - 每个数据槽的字节数；0 表示仅控制平面，无共享内存
 *   handler      - 服务端处理函数
 *   echo_fn      - 客户端输出回调（无需特殊格式则为 NULL）
 *
 * shm_obj_id 由框架在 fluxipc_server_init() 时按注册顺序自动分配，
 * 用户无需关心，也不会出现手动编号冲突。
 */
#define REGISTER_FLUXIPC(cmd_name, usage_str, slot_sz, func, echo_fn) \
    static fluxipc_entry_t _fluxipc_entry_##func FLUXIPC_SECTION = { \
        .name         = (cmd_name),                                      \
        .usage        = (usage_str),                                     \
        .shm_obj_id   = 0,           /* 由框架启动时填充 */             \
        .slot_data_sz = (size_t)(slot_sz),                               \
        .handler      = (func),                                          \
        .echo         = (echo_fn),                                       \
    }

/* ── 公开 API ───────────────────────────────────────────── */

/* 打印帮助信息：框架模式说明 + 所有注册的 IPC 命令 */
void fluxipc_usage(const char *prog);

/* 客户端模式：连接服务端并执行一条 IPC 命令。
 * argv[0] 为命令名，argv[1..] 为命令参数。 */
int fluxipc_client_init(int argc, char **argv);

/* 交互式客户端模式：启动 readline 交互 shell */
int fluxipc_interactive_init(int argc, char **argv);

/* 服务端模式：创建并启动 IPC 服务端。
 * prog_name 为程序名（通常传入 argv[0]），用于日志显示。 */
int fluxipc_server_init(const char *prog_name);

int  fluxipc_poll(void *data);
void fluxipc_destroy(void);
void fluxipc_stop(void);