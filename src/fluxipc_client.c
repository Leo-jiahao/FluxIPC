#include "fluxipc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define FLUXIPC_RELEASE_CMD  0xFFFFFFFFu

struct fluxipc_client {
    int       fd;
    uint32_t  seq;
    fluxipc_shm_t shm;
    int       shm_ok;
    char      server_name[64];
};

/* ── 辅助函数 ─────────────────────────────────────────── */

static ssize_t read_exact(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0) return r == 0 ? 0 : -errno;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

static ssize_t write_exact(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const char *)buf + done, n - done);
        if (w <= 0) return w == 0 ? 0 : -errno;
        done += (size_t)w;
    }
    return (ssize_t)done;
}

/* ── 连接 / 断开 ──────────────────────────────────────── */

fluxipc_client_t *fluxipc_client_connect(const char *server_name)
{
    struct fluxipc_client *cli = calloc(1, sizeof(*cli));
    if (!cli) return NULL;

    strncpy(cli->server_name, server_name, sizeof(cli->server_name) - 1);
    cli->fd     = -1;
    cli->seq    = 1;
    cli->shm_ok = 0;

    /* Unix socket */
    char path[128];
    snprintf(path, sizeof(path), FLUXIPC_SOCKET_PATH_FMT, server_name);

    cli->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cli->fd < 0) { free(cli); return NULL; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "fluxipc_client: socket path too long: %s\n", path);
        close(cli->fd);
        free(cli);
        return NULL;
    }
    strcpy(addr.sun_path, path);

    if (connect(cli->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(cli->fd);
        free(cli);
        return NULL;
    }

    /* 附加共享内存 */
    if (fluxipc_shm_open(&cli->shm, server_name) == 0) {
        cli->shm_ok = 1;
    } else {
        fprintf(stderr, "[client] warning: could not attach shm\n");
    }

    return cli;
}

void fluxipc_client_disconnect(fluxipc_client_t *cli)
{
    if (!cli) return;
    if (cli->fd >= 0)  close(cli->fd);
    if (cli->shm_ok)   fluxipc_shm_close(&cli->shm);
    free(cli);
}

/* ── fluxipc_client_call ──────────────────────────────── */

int fluxipc_client_call(fluxipc_client_t  *cli,
                    const char    *cmd,
                    int            argc,
                    char         **argv,
                    fluxipc_rsp_t     *rsp_out,
                    void         **data_out,
                    size_t        *data_len_out)
{
    if (data_out)     *data_out     = NULL;
    if (data_len_out) *data_len_out = 0;

    /* 在注册表中查找命令获取 cmd_id */
    uint32_t cmd_id = UINT32_MAX;
    uint32_t idx = 0;
    for (fluxipc_entry_t *e = __start_fluxipc_registry;
         e < __stop_fluxipc_registry; e++, idx++) {
        if (strcmp(e->name, cmd) == 0) {
            cmd_id = idx;
            break;
        }
    }
    if (cmd_id == UINT32_MAX) {
        fprintf(stderr, "[client] unknown command: %s\n", cmd);
        return -ENOENT;
    }

    /* 将 argv 打包到数据缓冲区 */
    size_t data_len = 0;
    for (int i = 0; i < argc; i++)
        data_len += strlen(argv[i]) + 1;

    char *data_buf = NULL;
    if (data_len > 0) {
        data_buf = malloc(data_len);
        if (!data_buf) return -ENOMEM;
        char *p = data_buf;
        for (int i = 0; i < argc; i++) {
            size_t l = strlen(argv[i]) + 1;
            memcpy(p, argv[i], l);
            p += l;
        }
    }

    /* 构造请求 */
    size_t req_sz = sizeof(fluxipc_req_t) + data_len;
    fluxipc_req_t *req = malloc(req_sz);
    if (!req) { free(data_buf); return -ENOMEM; }

    req->cmd_id   = cmd_id;
    req->seq      = cli->seq++;
    req->argc     = (uint32_t)argc;
    req->data_len = (uint32_t)data_len;
    if (data_len)
        memcpy(req->data, data_buf, data_len);
    free(data_buf);

    /* 发送 */
    if (write_exact(cli->fd, req, req_sz) != (ssize_t)req_sz) {
        free(req);
        return -EIO;
    }
    free(req);

    /* 接收响应 */
    fluxipc_rsp_t rsp;
    if (read_exact(cli->fd, &rsp, sizeof(rsp)) != (ssize_t)sizeof(rsp))
        return -EIO;

    if (rsp_out)
        *rsp_out = rsp;

    if (rsp.status != 0)
        return rsp.status;

    /* 响应包含数据平面槽位时获取它 */
    if (rsp.shm_obj_id != 0 && data_out) {
        if (!cli->shm_ok) {
            fprintf(stderr, "[client] shm not available\n");
            return -ENODEV;
        }
        fluxipc_slot_pool_hdr_t *pool =
            fluxipc_shm_pool_for(&cli->shm, rsp.shm_obj_id);
        if (!pool) {
            fprintf(stderr, "[client] pool not found for obj_id=%u\n",
                    rsp.shm_obj_id);
            return -ENODEV;
        }
        if (fluxipc_slot_acquire(pool, rsp.slot_idx, rsp.slot_gen) < 0) {
            fprintf(stderr, "[client] slot_acquire failed\n");
            return -ESTALE;
        }
        *data_out = fluxipc_slot_data_ptr(pool, rsp.slot_idx);
        if (data_len_out)
            *data_len_out = rsp.data_size;
    }

    return 0;
}

/* ── fluxipc_client_release ───────────────────────────── */

int fluxipc_client_release(fluxipc_client_t *cli, const fluxipc_rsp_t *rsp)
{
    if (!rsp || rsp->shm_obj_id == 0)
        return 0;

    /* 释放共享内存中的引用计数 */
    if (cli->shm_ok) {
        fluxipc_slot_pool_hdr_t *pool =
            fluxipc_shm_pool_for(&cli->shm, rsp->shm_obj_id);
        if (pool)
            fluxipc_slot_release(pool, rsp->slot_idx, rsp->slot_gen);
    }

    /* 通知服务端以便回收槽位 */
    fluxipc_rsp_t msg = *rsp;
    msg.cmd_id = FLUXIPC_RELEASE_CMD;
    write_exact(cli->fd, &msg, sizeof(msg));

    return 0;
}
