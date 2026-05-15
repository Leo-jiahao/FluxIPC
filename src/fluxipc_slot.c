#include "fluxipc_slot.h"
#include <string.h>
#include <errno.h>
#include <stdint.h>

/* ── 辅助函数 ─────────────────────────────────────────── */

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

fluxipc_slot_hdr_t *fluxipc_slot_at(fluxipc_slot_pool_hdr_t *pool, uint32_t idx)
{
    uint8_t *base = (uint8_t *)(pool + 1);
    return (fluxipc_slot_hdr_t *)(base + (size_t)idx * pool->slot_stride);
}

void *fluxipc_slot_data_ptr(fluxipc_slot_pool_hdr_t *pool, uint32_t idx)
{
    uint8_t *slot_base = (uint8_t *)fluxipc_slot_at(pool, idx);
    return slot_base + pool->data_offset;
}

size_t fluxipc_slot_pool_required_size(uint32_t num_slots, size_t slot_data_sz)
{
    size_t stride = align_up(sizeof(fluxipc_slot_hdr_t) + slot_data_sz, 64);
    return sizeof(fluxipc_slot_pool_hdr_t) + (size_t)num_slots * stride;
}

/* ── 初始化 / 附加 ────────────────────────────────────── */

fluxipc_slot_pool_hdr_t *fluxipc_slot_pool_init(void    *shm_base,
                                         size_t   region_size,
                                         uint32_t num_slots,
                                         size_t   slot_data_sz)
{
    (void)region_size;
    fluxipc_slot_pool_hdr_t *pool = (fluxipc_slot_pool_hdr_t *)shm_base;

    size_t stride = align_up(sizeof(fluxipc_slot_hdr_t) + slot_data_sz, 64);

    pool->magic       = FLUXIPC_SLOT_MAGIC;
    pool->num_slots   = num_slots;
    pool->slot_stride = stride;
    pool->data_offset = sizeof(fluxipc_slot_hdr_t);

    for (uint32_t i = 0; i < num_slots; i++) {
        fluxipc_slot_hdr_t *s = fluxipc_slot_at(pool, i);
        memset(s, 0, sizeof(*s));

        pthread_mutexattr_t ma;
        pthread_mutexattr_init(&ma);
        pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
        pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
        pthread_mutex_init(&s->lock, &ma);
        pthread_mutexattr_destroy(&ma);

        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
        pthread_cond_init(&s->cond, &ca);
        pthread_condattr_destroy(&ca);

        s->state = FLUXIPC_SLOT_FREE;
        s->gen   = 1;
    }

    return pool;
}

fluxipc_slot_pool_hdr_t *fluxipc_slot_pool_attach(void *shm_base)
{
    fluxipc_slot_pool_hdr_t *pool = (fluxipc_slot_pool_hdr_t *)shm_base;
    if (pool->magic != FLUXIPC_SLOT_MAGIC)
        return NULL;
    return pool;
}

/* ── 槽操作 ──────────────────────────────────────────── */

int fluxipc_slot_alloc(fluxipc_slot_pool_hdr_t *pool, uint32_t owner_seq,
                   uint32_t *gen_out)
{
    for (uint32_t i = 0; i < pool->num_slots; i++) {
        fluxipc_slot_hdr_t *s = fluxipc_slot_at(pool, i);
        pthread_mutex_lock(&s->lock);
        if (s->state == FLUXIPC_SLOT_FREE) {
            s->state     = FLUXIPC_SLOT_WRITING;
            s->gen++;
            s->refcnt    = 1;   /* 服务端写时持有一个引用 */
            s->owner_seq = owner_seq;
            s->data_len  = 0;
            if (gen_out)
                *gen_out = s->gen;
            pthread_mutex_unlock(&s->lock);
            return (int)i;
        }
        pthread_mutex_unlock(&s->lock);
    }
    return -1;  /* 池已满 */
}

int fluxipc_slot_mark_ready(fluxipc_slot_pool_hdr_t *pool, uint32_t idx,
                         size_t data_len)
{
    if (idx >= pool->num_slots)
        return -EINVAL;

    fluxipc_slot_hdr_t *s = fluxipc_slot_at(pool, idx);
    pthread_mutex_lock(&s->lock);

    if (s->state != FLUXIPC_SLOT_WRITING) {
        pthread_mutex_unlock(&s->lock);
        return -EINVAL;
    }

    s->data_len = data_len;
    s->state    = FLUXIPC_SLOT_READY;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int fluxipc_slot_acquire(fluxipc_slot_pool_hdr_t *pool, uint32_t idx, uint32_t gen)
{
    if (idx >= pool->num_slots)
        return -EINVAL;

    fluxipc_slot_hdr_t *s = fluxipc_slot_at(pool, idx);
    pthread_mutex_lock(&s->lock);

    if (s->gen != gen) {
        pthread_mutex_unlock(&s->lock);
        return -ESTALE;
    }
    if (s->state != FLUXIPC_SLOT_READY) {
        pthread_mutex_unlock(&s->lock);
        return -EAGAIN;
    }

    s->refcnt++;
    s->state = FLUXIPC_SLOT_CONSUMING;

    /* 释放服务端的写入引用 */
    s->refcnt--;

    pthread_mutex_unlock(&s->lock);
    return 0;
}

int fluxipc_slot_release(fluxipc_slot_pool_hdr_t *pool, uint32_t idx, uint32_t gen)
{
    if (idx >= pool->num_slots)
        return -EINVAL;

    fluxipc_slot_hdr_t *s = fluxipc_slot_at(pool, idx);
    pthread_mutex_lock(&s->lock);

    if (s->gen != gen) {
        pthread_mutex_unlock(&s->lock);
        return -ESTALE;
    }

    if (s->refcnt > 0)
        s->refcnt--;

    if (s->refcnt == 0) {
        s->state = FLUXIPC_SLOT_FREE;
        pthread_cond_broadcast(&s->cond);
    }

    pthread_mutex_unlock(&s->lock);
    return 0;
}
