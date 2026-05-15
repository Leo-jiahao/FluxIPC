#pragma once

/*
 * FluxIPC 内部头文件 — 非公开 API，仅供库内部源文件使用。
 */

#include "fluxipc.h"
#include "fluxipc_shm.h"

/* ── 常量 ───────────────────────────────────────────────── */

#define FLUXIPC_VERSION          1
#define FLUXIPC_SOCKET_PATH_FMT  "/tmp/fluxipc_%s.sock"
#define FLUXIPC_MAX_ARGS         32
#define FLUXIPC_RECV_TIMEOUT_MS  5000
#define FLUXIPC_DEFAULT_SLOTS    8

/* ── 通信协议类型 ──────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t cmd_id;
    uint32_t seq;
    uint32_t argc;
    uint32_t data_len;
    char     data[];
} fluxipc_req_t;

typedef struct __attribute__((packed)) {
    uint32_t cmd_id;
    uint32_t seq;
    int32_t  status;
    uint32_t shm_obj_id;
    uint32_t slot_idx;
    uint32_t slot_gen;
    uint32_t data_size;
} fluxipc_rsp_t;

/* ── 服务端 API（内部）─────────────────────────────────── */

typedef struct fluxipc_server fluxipc_server_t;

fluxipc_server_t *fluxipc_server_create(const char *name);
int               fluxipc_server_run(fluxipc_server_t *srv);
void              fluxipc_server_stop(fluxipc_server_t *srv);
void              fluxipc_server_destroy(fluxipc_server_t *srv);

/* ── 客户端 API（内部）─────────────────────────────────── */

typedef struct fluxipc_client fluxipc_client_t;

fluxipc_client_t *fluxipc_client_connect(const char *server_name);
void              fluxipc_client_disconnect(fluxipc_client_t *cli);

int fluxipc_client_call(fluxipc_client_t *cli,
                        const char   *cmd,
                        int           argc,
                        char        **argv,
                        fluxipc_rsp_t    *rsp_out,
                        void        **data_out,
                        size_t       *data_len_out);

int fluxipc_client_release(fluxipc_client_t *cli, const fluxipc_rsp_t *rsp);

/* ── 注册表工具（内部）─────────────────────────────────── */

const fluxipc_entry_t *fluxipc_registry_find(const char *name);
void                   fluxipc_registry_dump_usage(void);
