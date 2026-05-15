#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * FluxIPC — 统一 IPC 库
 *
 * 只需包含此头文件即可使用。从 main() 调用 fluxipc_init(argc, argv)，
 * 库根据二进制名称和参数自动识别角色：
 *
 *   角色          触发条件
 *   ────          ──────
 *   server        名称不含 "cli"/"cmd"/"client"
 *   client        名称含 "cli"/"cmd"/"client"，或 argv[1] == "--fic <command>"
 *   interactive   argv[1] == "--flc"
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
    uint32_t            shm_obj_id;    /* 0 = 无数据平面 */
    size_t              slot_data_sz;
    fluxipc_handler_fn  handler;
    fluxipc_echo_fn     echo;          /* 客户端输出回调，可为 NULL */
} fluxipc_entry_t;

/* 链接器提供的段边界 */
extern fluxipc_entry_t __start_fluxipc_registry[];
extern fluxipc_entry_t __stop_fluxipc_registry[];

#define FLUXIPC_SECTION \
    __attribute__((section("fluxipc_registry"), used, aligned(sizeof(void *))))

/*
 * REGISTER_FLUXIPC(cmd_name, usage_str, shm_obj_id, slot_data_sz, handler, echo_fn)
 *
 *   cmd_name     - 客户端发送的命令字符串（如 "get_stats"）
 *   usage_str    - 帮助文本
 *   shm_obj_id   - 数据平面命令的非零唯一 ID；0 表示仅控制平面
 *   slot_data_sz - 每个数据槽的字节数（shm_obj_id == 0 时忽略）
 *   handler      - 服务端处理函数
 *   echo_fn      - 客户端输出回调（无需特殊格式则为 NULL）
 */
#define REGISTER_FLUXIPC(cmd_name, usage_str, shm_id, slot_sz, func, echo_fn) \
    static fluxipc_entry_t _fluxipc_entry_##func FLUXIPC_SECTION = { \
        .name         = (cmd_name),                                      \
        .usage        = (usage_str),                                     \
        .shm_obj_id   = (uint32_t)(shm_id),                             \
        .slot_data_sz = (size_t)(slot_sz),                               \
        .handler      = (func),                                          \
        .echo         = (echo_fn),                                       \
    }

/* ── 公开 API ───────────────────────────────────────────── */

int  fluxipc_init(int argc, char **argv);
int  fluxipc_poll(void *data);
void fluxipc_destroy(void);
void fluxipc_stop(void);
