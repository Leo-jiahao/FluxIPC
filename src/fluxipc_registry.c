#include "fluxipc_internal.h"
#include <stdio.h>
#include <string.h>

/*
 * fluxipc_registry_assign_ids —— 为所有 slot_data_sz > 0 的 entry 自动分配
 * 唯一 shm_obj_id（从 1 开始递增）。
 *
 * 必须在 fluxipc_shm_create() 和 fluxipc_registry_dump_usage() 之前调用。
 * 幂等：已有非零 id 的 entry 不会被重新编号。
 */
void fluxipc_registry_assign_ids(void)
{
    uint32_t next_id = 1;

    /* 先找到当前最大已用 id，避免与任何手动指定的 id 冲突 */
    for (fluxipc_entry_t *e = __start_fluxipc_registry;
         e < __stop_fluxipc_registry; e++) {
        if (e->shm_obj_id >= next_id)
            next_id = e->shm_obj_id + 1;
    }

    /* 再为尚未分配 id 的 data-plane entry 填入递增 id */
    for (fluxipc_entry_t *e = __start_fluxipc_registry;
         e < __stop_fluxipc_registry; e++) {
        if (e->slot_data_sz > 0 && e->shm_obj_id == 0)
            e->shm_obj_id = next_id++;
    }
}

const fluxipc_entry_t *fluxipc_registry_find(const char *name)
{
    for (fluxipc_entry_t *e = __start_fluxipc_registry;
         e < __stop_fluxipc_registry; e++) {
        if (strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

void fluxipc_registry_dump_usage(void)
{
    printf("Registered IPC commands:\n");
    for (fluxipc_entry_t *e = __start_fluxipc_registry;
         e < __stop_fluxipc_registry; e++) {
        printf("  %-20s  %s\n", e->name, e->usage ? e->usage : "");
        if (e->slot_data_sz > 0)
            printf("  %20s  [data-plane: obj_id=%u (auto)  slot=%zu bytes]\n",
                   "", e->shm_obj_id, e->slot_data_sz);
    }
}