#include "fluxipc_internal.h"
#include <stdio.h>
#include <string.h>

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
        if (e->shm_obj_id)
            printf("  %20s  [data-plane: obj_id=%u  slot=%zu bytes]\n",
                   "", e->shm_obj_id, e->slot_data_sz);
    }
}
