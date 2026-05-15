#pragma once

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/*
 * 槽位状态机：
 *
 *   FREE ──alloc──► WRITING ──mark_ready──► READY
 *    ▲                                        │
 *    └──────release (refcnt→0)────── CONSUMING ◄─ client_acquire
 *
 * 多个并发客户端各自获取独立槽位，互不覆盖。
 */

typedef enum {
    FLUXIPC_SLOT_FREE      = 0,
    FLUXIPC_SLOT_WRITING   = 1,
    FLUXIPC_SLOT_READY     = 2,
    FLUXIPC_SLOT_CONSUMING = 3,
} fluxipc_slot_state_t;

/*
 * 单槽头部，其后紧跟 data[slot_data_sz]。
 * 完全位于共享内存段内，mutex/cond 为 PTHREAD_PROCESS_SHARED。
 */
typedef struct {
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    fluxipc_slot_state_t state;
    uint32_t         gen;        /* 单调递增，每次分配时 +1 */
    uint32_t         refcnt;
    uint32_t         owner_seq;  /* 占用此槽的请求序号 */
    size_t           data_len;   /* 处理函数写入的字节数 */
    /* uint8_t data[slot_data_sz] 紧随其后 */
} fluxipc_slot_hdr_t;

/*
 * 池头部，位于每个对象子区域的起始处。
 */
typedef struct {
    uint32_t magic;        /* FLUXIPC_SLOT_MAGIC */
    uint32_t num_slots;
    size_t   slot_stride;  /* sizeof(fluxipc_slot_hdr_t) + slot_data_sz，对齐后 */
    size_t   data_offset;  /* 从槽基址到数据区的偏移 */
} fluxipc_slot_pool_hdr_t;

#define FLUXIPC_SLOT_MAGIC  0x534C5400u  /* "SLT\0" */

/* ── 池 API ─────────────────────────────────────────── */

/*
 * 在给定内存地址初始化池（服务端调用一次）。
 * 返回池头部指针（== shm_base）。
 */
fluxipc_slot_pool_hdr_t *fluxipc_slot_pool_init(void    *shm_base,
                                         size_t   region_size,
                                         uint32_t num_slots,
                                         size_t   slot_data_sz);

/* 附加到已初始化的池（客户端调用）。 */
fluxipc_slot_pool_hdr_t *fluxipc_slot_pool_attach(void *shm_base);

/*
 * 计算给定参数下池所需的字节数。
 */
size_t fluxipc_slot_pool_required_size(uint32_t num_slots, size_t slot_data_sz);

/* ── 槽操作 ─────────────────────────────────────────── */

/* 分配空闲槽；返回索引 >= 0，池满返回 -1。 */
int fluxipc_slot_alloc(fluxipc_slot_pool_hdr_t *pool, uint32_t owner_seq,
                   uint32_t *gen_out);

/* 返回 slot[idx] 数据区指针（不做校验）。 */
void *fluxipc_slot_data_ptr(fluxipc_slot_pool_hdr_t *pool, uint32_t idx);

/* 服务端：写入 data_len 字节后将槽标记为就绪。 */
int fluxipc_slot_mark_ready(fluxipc_slot_pool_hdr_t *pool, uint32_t idx,
                         size_t data_len);

/* 客户端：获取槽进行读取（递增 refcnt，状态 → CONSUMING）。 */
int fluxipc_slot_acquire(fluxipc_slot_pool_hdr_t *pool, uint32_t idx, uint32_t gen);

/* 客户端：释放槽（递减 refcnt；归零时释放）。 */
int fluxipc_slot_release(fluxipc_slot_pool_hdr_t *pool, uint32_t idx, uint32_t gen);

/* 内部辅助：按索引获取槽头部 */
fluxipc_slot_hdr_t *fluxipc_slot_at(fluxipc_slot_pool_hdr_t *pool, uint32_t idx);
