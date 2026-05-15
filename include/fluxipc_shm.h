#pragma once

#include <stdint.h>
#include <stddef.h>
#include "fluxipc_slot.h"

/*
 * 共享内存段布局：
 *
 *   偏移 0:
 *     fluxipc_shm_global_t          （全局头）
 *   偏移 sizeof(fluxipc_shm_global_t)，对齐后:
 *     fluxipc_obj_desc_t[num_objs]  （对象描述符表）
 *   随后是每个对象的区域（各自对齐到 FLUXIPC_SHM_ALIGN）:
 *     [obj_0: fluxipc_slot_pool_hdr_t + N * slot_stride]
 *     [obj_1: ...]
 *     ...
 *
 * 段名格式："/fluxipc_<server_name>"
 */

#define FLUXIPC_SHM_MAGIC    0x464C5043u  /* "FLPC" */
#define FLUXIPC_SHM_VERSION  1u
#define FLUXIPC_SHM_ALIGN    64u          /* 缓存行对齐 */
#define FLUXIPC_SHM_NAME_FMT "/fluxipc_%s"

/* 单个 IPC 对象的子区域描述符 */
typedef struct {
    uint32_t obj_id;        /* 对应 fluxipc_entry_t.shm_obj_id */
    uint32_t num_slots;
    size_t   slot_data_sz;
    size_t   region_offset; /* 从段起始的字节偏移 */
    size_t   region_size;
} fluxipc_obj_desc_t;

/* 段偏移 0 处的全局头 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_objs;
    size_t   total_size;
    /* fluxipc_obj_desc_t descs[num_objs] 紧随其后 */
} fluxipc_shm_global_t;

/* 运行时句柄（不存入共享内存） */
typedef struct {
    int                  fd;
    size_t               total_size;
    void                *base;
    fluxipc_shm_global_t    *hdr;
    /* 池指针，按 descs[] 数组位置索引 */
    uint32_t             num_pools;
    fluxipc_slot_pool_hdr_t *pools[64];   /* 最多 64 个注册对象 */
} fluxipc_shm_t;

/* ── API ────────────────────────────────────────────── */

/*
 * 服务端：扫描 fluxipc_registry 段，计算布局，创建并初始化共享内存段。
 * num_slots: 每个对象的槽位数（0 则使用 FLUXIPC_DEFAULT_SLOTS）。
 */
int  fluxipc_shm_create(fluxipc_shm_t *shm, const char *server_name,
                    uint32_t num_slots);

/*
 * 客户端：打开已有段并映射所有池。
 */
int  fluxipc_shm_open(fluxipc_shm_t *shm, const char *server_name);

void fluxipc_shm_close(fluxipc_shm_t *shm);

/* 服务端：删除段名 */
void fluxipc_shm_unlink(const char *server_name);

/*
 * 按 obj_id 查找对应池，未找到返回 NULL。
 */
fluxipc_slot_pool_hdr_t *fluxipc_shm_pool_for(fluxipc_shm_t *shm, uint32_t obj_id);
