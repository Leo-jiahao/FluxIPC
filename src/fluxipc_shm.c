#include "fluxipc_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ── 辅助函数 ─────────────────────────────────────────── */

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static void shm_name(char *buf, size_t sz, const char *server_name)
{
    snprintf(buf, sz, FLUXIPC_SHM_NAME_FMT, server_name);
}

/* ── fluxipc_shm_create ───────────────────────────────── */

/*
 * 扫描 fluxipc_registry 段，为每个 slot_data_sz > 0 的 entry 自动分配
 * 递增的 shm_obj_id（从 1 开始），收集唯一对象信息，计算段总大小，
 * 创建 + mmap，初始化所有槽池。
 *
 * 自动编号规则：
 *   - 按 ELF 段中注册顺序遍历
 *   - slot_data_sz == 0 的 entry 跳过（纯控制平面命令）
 *   - 同一 shm_obj_id 可被多条命令共享（框架不拆分同 id 的 entry），
 *     但在自动模式下每条有 slot_data_sz > 0 的 entry 各自独立获得唯一 id
 */
int fluxipc_shm_create(fluxipc_shm_t *shm, const char *server_name,
                   uint32_t num_slots)
{
    memset(shm, 0, sizeof(*shm));

    if (num_slots == 0)
        num_slots = FLUXIPC_DEFAULT_SLOTS;

    /* ── 第一遍：收集唯一 obj_id（assign_ids 已提前完成编号） ── */

    typedef struct { uint32_t obj_id; size_t slot_data_sz; } obj_info_t;
    obj_info_t objs[64];
    uint32_t num_objs = 0;
    uint32_t next_id  = 1;   /* 仅用于兼容路径，正常情况 assign_ids 已填好 */

    for (fluxipc_entry_t *e = __start_fluxipc_registry;
         e < __stop_fluxipc_registry; e++) {

        if (e->slot_data_sz == 0) {
            e->shm_obj_id = 0;
            continue;
        }

        /* 防御：若 assign_ids 未被调用，此处补充分配 */
        if (e->shm_obj_id == 0)
            e->shm_obj_id = next_id++;
        else if (e->shm_obj_id >= next_id)
            next_id = e->shm_obj_id + 1;

        /* 收集唯一 obj_id，同 id 取最大 slot_data_sz */
        int found = 0;
        for (uint32_t i = 0; i < num_objs; i++) {
            if (objs[i].obj_id == e->shm_obj_id) {
                found = 1;
                if (e->slot_data_sz > objs[i].slot_data_sz)
                    objs[i].slot_data_sz = e->slot_data_sz;
                break;
            }
        }
        if (!found && num_objs < 64) {
            objs[num_objs].obj_id       = e->shm_obj_id;
            objs[num_objs].slot_data_sz = e->slot_data_sz;
            num_objs++;
        }
    }

    /* ── 第二遍：计算总大小 ── */

    /* 全局头 */
    size_t off = align_up(sizeof(fluxipc_shm_global_t), FLUXIPC_SHM_ALIGN);
    /* 对象描述符表 */
    off = align_up(off + sizeof(fluxipc_obj_desc_t) * num_objs, FLUXIPC_SHM_ALIGN);

    /* 每个对象的区域 */
    size_t region_offsets[64];
    size_t region_sizes[64];
    for (uint32_t i = 0; i < num_objs; i++) {
        region_offsets[i] = off;
        region_sizes[i]   = align_up(
            fluxipc_slot_pool_required_size(num_slots, objs[i].slot_data_sz),
            FLUXIPC_SHM_ALIGN);
        off += region_sizes[i];
    }

    size_t total = off;

    /* ── 创建共享内存段 ── */

    char name[128];
    shm_name(name, sizeof(name), server_name);

    shm_unlink(name);  /* 清理残留 */

    int fd = shm_open(name, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        perror("shm_open(create)");
        return -errno;
    }

    if (ftruncate(fd, (off_t)total) < 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(name);
        return -errno;
    }

    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(name);
        return -errno;
    }

    memset(base, 0, total);

    /* ── 写入全局头 ── */

    fluxipc_shm_global_t *hdr = (fluxipc_shm_global_t *)base;
    hdr->version    = FLUXIPC_SHM_VERSION;
    hdr->num_objs   = num_objs;
    hdr->total_size = total;

    /* ── 写入对象描述符 ── */

    fluxipc_obj_desc_t *descs = (fluxipc_obj_desc_t *)
        ((uint8_t *)base + align_up(sizeof(fluxipc_shm_global_t), FLUXIPC_SHM_ALIGN));

    for (uint32_t i = 0; i < num_objs; i++) {
        descs[i].obj_id        = objs[i].obj_id;
        descs[i].num_slots     = num_slots;
        descs[i].slot_data_sz  = objs[i].slot_data_sz;
        descs[i].region_offset = region_offsets[i];
        descs[i].region_size   = region_sizes[i];
    }

    /* ── 初始化槽池 ── */

    shm->num_pools = num_objs;
    for (uint32_t i = 0; i < num_objs; i++) {
        void *region = (uint8_t *)base + region_offsets[i];
        shm->pools[i] = fluxipc_slot_pool_init(region, region_sizes[i],
                                            num_slots, objs[i].slot_data_sz);
        if (!shm->pools[i]) {
            fprintf(stderr, "fluxipc_slot_pool_init failed for obj %u\n",
                    objs[i].obj_id);
            munmap(base, total);
            close(fd);
            shm_unlink(name);
            return -EINVAL;
        }
    }

    /* 最后写入 magic（向客户端标识段已就绪） */
    hdr->magic = FLUXIPC_SHM_MAGIC;

    shm->fd         = fd;
    shm->total_size = total;
    shm->base       = base;
    shm->hdr        = hdr;

    printf("[shm] created '%s'  total=%zu bytes  %u objects  %u slots each\n",
           name, total, num_objs, num_slots);
    for (uint32_t i = 0; i < num_objs; i++) {
        printf("  obj_id=%-4u  offset=%-8zu  size=%-8zu  slot_data=%zu\n",
               descs[i].obj_id, descs[i].region_offset,
               descs[i].region_size, descs[i].slot_data_sz);
    }

    return 0;
}

/* ── fluxipc_shm_open ─────────────────────────────────── */

int fluxipc_shm_open(fluxipc_shm_t *shm, const char *server_name)
{
    memset(shm, 0, sizeof(*shm));

    char name[128];
    shm_name(name, sizeof(name), server_name);

    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0)
        return -errno;

    /* 先读取全局头 */
    fluxipc_shm_global_t ghdr;
    if (read(fd, &ghdr, sizeof(ghdr)) != (ssize_t)sizeof(ghdr)) {
        close(fd);
        return -EIO;
    }

    if (ghdr.magic != FLUXIPC_SHM_MAGIC) {
        close(fd);
        return -EINVAL;
    }

    void *base = mmap(NULL, ghdr.total_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return -errno;
    }

    fluxipc_shm_global_t *hdr = (fluxipc_shm_global_t *)base;
    fluxipc_obj_desc_t   *descs = (fluxipc_obj_desc_t *)
        ((uint8_t *)base + align_up(sizeof(fluxipc_shm_global_t), FLUXIPC_SHM_ALIGN));

    shm->fd         = fd;
    shm->total_size = hdr->total_size;
    shm->base       = base;
    shm->hdr        = hdr;
    shm->num_pools  = hdr->num_objs;

    for (uint32_t i = 0; i < hdr->num_objs; i++) {
        void *region = (uint8_t *)base + descs[i].region_offset;
        shm->pools[i] = fluxipc_slot_pool_attach(region);
        if (!shm->pools[i]) {
            fprintf(stderr, "[shm] pool attach failed obj_id=%u\n",
                    descs[i].obj_id);
            munmap(base, ghdr.total_size);
            close(fd);
            return -EINVAL;
        }
    }

    return 0;
}

/* ── fluxipc_shm_pool_for ─────────────────────────────── */

fluxipc_slot_pool_hdr_t *fluxipc_shm_pool_for(fluxipc_shm_t *shm, uint32_t obj_id)
{
    if (!shm->base)
        return NULL;

    fluxipc_obj_desc_t *descs = (fluxipc_obj_desc_t *)
        ((uint8_t *)shm->base +
         align_up(sizeof(fluxipc_shm_global_t), FLUXIPC_SHM_ALIGN));

    for (uint32_t i = 0; i < shm->hdr->num_objs; i++) {
        if (descs[i].obj_id == obj_id)
            return shm->pools[i];
    }
    return NULL;
}

/* ── 清理 ────────────────────────────────────────────── */

void fluxipc_shm_close(fluxipc_shm_t *shm)
{
    if (shm->base && shm->base != MAP_FAILED)
        munmap(shm->base, shm->total_size);
    if (shm->fd >= 0)
        close(shm->fd);
    memset(shm, 0, sizeof(*shm));
}

void fluxipc_shm_unlink(const char *server_name)
{
    char name[128];
    shm_name(name, sizeof(name), server_name);
    shm_unlink(name);
}