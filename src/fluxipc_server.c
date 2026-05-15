#include "fluxipc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>

#define MAX_CLIENTS  256
#define EPOLL_EVENTS 64

struct fluxipc_server {
    char            name[64];
    int             listen_fd;
    int             epoll_fd;
    fluxipc_shm_t       shm;
    volatile int    running;
    /* 每个客户端的连接状态 */
    int             client_fds[MAX_CLIENTS];
};

/* ── 辅助函数 ─────────────────────────────────────────── */

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int epoll_add(int epfd, int fd, uint32_t events)
{
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_del(int epfd, int fd)
{
    return epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

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

/* ── 请求分发 ─────────────────────────────────────────── */

static void handle_request(struct fluxipc_server *srv, int client_fd)
{
    /* 先读取固定头部 */
    fluxipc_req_t hdr;
    ssize_t r = read_exact(client_fd, &hdr, sizeof(hdr));
    if (r <= 0) {
        if (r < 0)
            fprintf(stderr, "[server] read hdr: %s\n", strerror(errno));
        return;
    }

    /* 读取变长数据 */
    char *data = NULL;
    if (hdr.data_len > 0) {
        data = malloc(hdr.data_len + 1);
        r = read_exact(client_fd, data ? data : (char*)&r, hdr.data_len);
        if (!data || r <= 0) { free(data); return; }
        data[hdr.data_len] = '\0';
    }

    /* 提前准备响应，以便随时发送错误 */
    fluxipc_rsp_t rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.cmd_id = hdr.cmd_id;
    rsp.seq    = hdr.seq;

    /* 重建 argv */
    char *argv[FLUXIPC_MAX_ARGS + 1];
    uint32_t argc = 0;

    if (data && hdr.argc > 0) {
        char *p = data;
        for (uint32_t i = 0; i < hdr.argc && i < FLUXIPC_MAX_ARGS; i++) {
            argv[argc++] = p;
            p += strlen(p) + 1;
            if (p >= data + hdr.data_len) break;
        }
    }
    argv[argc] = NULL;

    /* 按 cmd_id（注册表索引）查找命令 */
    uint32_t num_cmds = (uint32_t)(__stop_fluxipc_registry - __start_fluxipc_registry);

    if (hdr.cmd_id >= num_cmds) {
        rsp.status = -ENOENT;
        write_exact(client_fd, &rsp, sizeof(rsp));
        free(data);
        return;
    }

    fluxipc_entry_t *entry = &__start_fluxipc_registry[hdr.cmd_id];

    /* 需要时分配数据平面槽位 */
    int slot_idx = -1;
    uint32_t slot_gen = 0;
    void *slot_data = NULL;
    fluxipc_slot_pool_hdr_t *pool = NULL;

    if (entry->shm_obj_id != 0) {
        pool = fluxipc_shm_pool_for(&srv->shm, entry->shm_obj_id);
        if (!pool) {
            fprintf(stderr, "[server] no pool for obj_id=%u\n",
                    entry->shm_obj_id);
            rsp.status = -ENODEV;
            write_exact(client_fd, &rsp, sizeof(rsp));
            free(data);
            return;
        }
        slot_idx = fluxipc_slot_alloc(pool, hdr.seq, &slot_gen);
        if (slot_idx < 0) {
            fprintf(stderr, "[server] pool full for obj_id=%u\n",
                    entry->shm_obj_id);
            rsp.status = -EBUSY;
            write_exact(client_fd, &rsp, sizeof(rsp));
            free(data);
            return;
        }
        slot_data = fluxipc_slot_data_ptr(pool, (uint32_t)slot_idx);
    }

    /* 调用处理函数 */
    int rc = entry->handler((int)argc, argv,
                             slot_data, entry->slot_data_sz);

    if (rc != 0) {
        rsp.status = rc;
        if (pool && slot_idx >= 0) {
            /* 失败则立即释放槽位 */
            fluxipc_slot_hdr_t *s = fluxipc_slot_at(pool, (uint32_t)slot_idx);
            pthread_mutex_lock(&s->lock);
            s->state  = FLUXIPC_SLOT_FREE;
            s->refcnt = 0;
            pthread_mutex_unlock(&s->lock);
        }
        write_exact(client_fd, &rsp, sizeof(rsp));
        free(data);
        return;
    }

    /* 标记槽位就绪 */
    if (pool && slot_idx >= 0) {
        /* 确定处理函数实际写入的数据大小 */
        size_t written = strnlen((char *)slot_data, entry->slot_data_sz);
        if (written == entry->slot_data_sz) written = entry->slot_data_sz;
        else written++;   /* 字符串则包含 NUL */

        fluxipc_slot_mark_ready(pool, (uint32_t)slot_idx, written);

        rsp.shm_obj_id = entry->shm_obj_id;
        rsp.slot_idx   = (uint32_t)slot_idx;
        rsp.slot_gen   = slot_gen;
        rsp.data_size  = (uint32_t)written;
    }

    write_exact(client_fd, &rsp, sizeof(rsp));
    free(data);
}

/* ── 客户端释放通知 ───────────────────────────────────── */

/*
 * 客户端读取槽位数据后发送一个小的 "释放" 消息：
 * 即 cmd_id = UINT32_MAX 的 fluxipc_rsp_t。
 */
#define FLUXIPC_RELEASE_CMD  0xFFFFFFFFu

static void handle_release(struct fluxipc_server *srv, const fluxipc_rsp_t *msg)
{
    if (msg->shm_obj_id == 0)
        return;
    fluxipc_slot_pool_hdr_t *pool = fluxipc_shm_pool_for(&srv->shm,
                                                   msg->shm_obj_id);
    if (!pool) return;
    fluxipc_slot_release(pool, msg->slot_idx, msg->slot_gen);
}

/* ── 服务端创建 / 销毁 ────────────────────────────────── */

fluxipc_server_t *fluxipc_server_create(const char *name)
{
    struct fluxipc_server *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    strncpy(srv->name, name, sizeof(srv->name) - 1);

    for (int i = 0; i < MAX_CLIENTS; i++)
        srv->client_fds[i] = -1;

    /* 创建共享内存 */
    if (fluxipc_shm_create(&srv->shm, name, FLUXIPC_DEFAULT_SLOTS) < 0) {
        free(srv);
        return NULL;
    }

    /* 创建 Unix socket */
    char path[128];
    snprintf(path, sizeof(path), FLUXIPC_SOCKET_PATH_FMT, name);
    unlink(path);

    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        perror("socket");
        fluxipc_shm_close(&srv->shm);
        free(srv);
        return NULL;
    }
    set_nonblock(srv->listen_fd);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "fluxipc_server: socket path too long: %s\n", path);
        close(srv->listen_fd);
        fluxipc_shm_close(&srv->shm);
        free(srv);
        return NULL;
    }
    strcpy(addr.sun_path, path);
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv->listen_fd, 64) < 0) {
        perror("bind/listen");
        close(srv->listen_fd);
        fluxipc_shm_close(&srv->shm);
        free(srv);
        return NULL;
    }

    /* 创建 epoll */
    srv->epoll_fd = epoll_create1(0);
    epoll_add(srv->epoll_fd, srv->listen_fd, EPOLLIN);

    printf("[server] '%s' listening on %s\n", name, path);
    return srv;
}

void fluxipc_server_stop(fluxipc_server_t *srv)
{
    if (srv) srv->running = 0;
}

void fluxipc_server_destroy(fluxipc_server_t *srv)
{
    if (!srv) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (srv->client_fds[i] >= 0) {
            epoll_del(srv->epoll_fd, srv->client_fds[i]);
            close(srv->client_fds[i]);
        }
    }
    if (srv->epoll_fd >= 0)  close(srv->epoll_fd);
    if (srv->listen_fd >= 0) close(srv->listen_fd);

    char path[128];
    snprintf(path, sizeof(path), FLUXIPC_SOCKET_PATH_FMT, srv->name);
    unlink(path);

    fluxipc_shm_close(&srv->shm);
    fluxipc_shm_unlink(srv->name);
    free(srv);
}

/* ── 主事件循环 ───────────────────────────────────────── */

int fluxipc_server_run(fluxipc_server_t *srv)
{
    struct epoll_event events[EPOLL_EVENTS];
    srv->running = 1;

    while (srv->running) {
        int n = epoll_wait(srv->epoll_fd, events, EPOLL_EVENTS, 200);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == srv->listen_fd) {
                /* 接受新客户端 */
                int cfd = accept(fd, NULL, NULL);
                if (cfd < 0) continue;
                set_nonblock(cfd);

                /* 查找空闲槽位 */
                int slot = -1;
                for (int j = 0; j < MAX_CLIENTS; j++) {
                    if (srv->client_fds[j] < 0) { slot = j; break; }
                }
                if (slot < 0) {
                    fprintf(stderr, "[server] too many clients\n");
                    close(cfd);
                    continue;
                }
                srv->client_fds[slot] = cfd;
                epoll_add(srv->epoll_fd, cfd, EPOLLIN | EPOLLET);
                printf("[server] client connected fd=%d\n", cfd);

            } else {
                /* 客户端数据 */
                if (!(events[i].events & EPOLLIN)) continue;

                /* Peek 区分请求和释放消息 */
                uint32_t peek_cmd;
                ssize_t p = recv(fd, &peek_cmd, sizeof(peek_cmd),
                                 MSG_PEEK | MSG_DONTWAIT);
                if (p <= 0) {
                    /* 客户端断开 */
                    printf("[server] client fd=%d disconnected\n", fd);
                    epoll_del(srv->epoll_fd, fd);
                    close(fd);
                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (srv->client_fds[j] == fd) {
                            srv->client_fds[j] = -1;
                            break;
                        }
                    }
                    continue;
                }

                if (peek_cmd == FLUXIPC_RELEASE_CMD) {
                    fluxipc_rsp_t msg;
                    read_exact(fd, &msg, sizeof(msg));
                    handle_release(srv, &msg);
                } else {
                    handle_request(srv, fd);
                }
            }
        }
    }

    return 0;
}
