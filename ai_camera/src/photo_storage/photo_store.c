/* SPDX-License-Identifier: Apache-2.0 */
#include "photo_store.h"

#include <dfs_posix.h>
#include <dfs_fs.h>
#include <drv_flash.h>
#include <bf0_hal.h>
#include <errno.h>
#include <fcntl.h>
#include <finsh.h>
#include <mem_map.h>
#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FatFs and POSIX both name their directory handle DIR. */
#define DIR PHOTO_STORE_FAT_DIR
#include <ff.h>
#undef DIR

#define DBG_TAG "camera.storage"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define PHOTO_STORE_DEVICE "root"
#define PHOTO_STORE_LEGACY_LATEST PHOTO_STORE_DIRECTORY "/latest.jpg"
#define PHOTO_STORE_IO_CHUNK 4096U

static struct rt_mutex s_lock;
static rt_bool_t s_lock_initialized;
static rt_bool_t s_ready;
static int s_last_error;
static uint32_t s_last_id;
static uint32_t s_cluster_bytes;

static rt_bool_t s_default_dma_configured;

/* 1 = drop the NOR DMA engine so program/erase is CPU driven (A/B test). */
#define PHOTO_STORE_NOR_DMA_DETACH 1

/* Volume signature: records which FS layout created the volume. The helpers are
 * defined with the rotation record code at the end of this file. */
static int photo_store_signature_layout_changed(int *changed);
static int photo_store_signature_write(void);

/* The caller holds photo_fs; inspect the SDK DMA handle under the flash lock. */
static int photo_store_configure_dma_locked(const char **reason)
{
    FLASH_HandleTypeDef *flash;
    DMA_HandleTypeDef *candidate;
    int result;

    *reason = "ok";
    flash = rt_flash_get_handle_by_addr(FLASH_TABLE_START_ADDR);
    if (flash == NULL || flash->Instance == NULL || flash->isNand != SPI_MODE_NOR ||
        flash->size == 0 || FLASH_TABLE_START_ADDR < flash->base ||
        (uint64_t)FLASH_TABLE_START_ADDR - flash->base >= flash->size)
    {
        *reason = "nor_handle";
        return -ENODEV;
    }

#if PHOTO_STORE_NOR_DMA_DETACH
    /* The handle itself stays valid; only the DMA engine is dropped, which
     * makes the HAL program/erase path CPU-driven. */
    rt_flash_lock(flash->base);
    flash->dma = NULL;
    rt_flash_unlock(flash->base);
    *reason = "dma_detached";
    return 0;
#endif

    rt_flash_lock(flash->base);
    result = 0;
    candidate = flash->dma;
    if (candidate == NULL)
    {
        result = -ENODEV;
        *reason = "dma_unavailable";
    }
    else if (candidate->Instance == NULL)
    {
        result = -ENODEV;
        *reason = "dma_handle";
    }
    else if ((flash->Instance->CR & MPI_CR_DMAE) != 0 ||
             candidate->Lock != HAL_UNLOCKED ||
             (candidate->State != HAL_DMA_STATE_READY &&
              candidate->State != HAL_DMA_STATE_RESET))
    {
        result = -EBUSY;
        *reason = "dma_not_idle";
    }
    else if (candidate->ErrorCode != HAL_DMA_ERROR_NONE)
    {
        result = -EIO;
        *reason = "dma_error";
    }
    else if (candidate->Init.Priority != DMA_PRIORITY_VERY_HIGH)
    {
        candidate->Init.Priority = DMA_PRIORITY_VERY_HIGH;
        if (HAL_DMA_Init(candidate) != HAL_OK)
        {
            result = -EIO;
            *reason = "dma_priority";
        }
    }
    /* Keep the initialized handle attached so NOR writes use DMA. */
    rt_flash_unlock(flash->base);
    return result;
}

static int photo_store_errno(void)
{
    int error = errno;

    return error == 0 ? -EIO : (error < 0 ? error : -error);
}

static int photo_store_error(const char *step, int result)
{
    LOG_E("step=%s error=%d", step, result);
    return result;
}

static int photo_store_posix_error(const char *step)
{
    return photo_store_error(step, photo_store_errno());
}

static int photo_store_validate_region(void)
{
    FLASH_HandleTypeDef *flash;
    const uint64_t start = FS_REGION_START_ADDR;
    const uint64_t size = FS_REGION_SIZE;
    uint32_t flash_id;
    int flash_bytes;

    flash = rt_flash_get_handle_by_addr(FLASH_TABLE_START_ADDR);
    if (flash == NULL)
        return photo_store_error("nor_handle", -ENODEV);
    flash_id = (uint32_t)rt_flash_read_id(FLASH_TABLE_START_ADDR);
    flash_bytes = rt_flash_get_total_size(FLASH_TABLE_START_ADDR);
    if (flash->isNand != SPI_MODE_NOR || flash_bytes <= 0 ||
        (flash_id & 0x00ffffffU) == 0 || (flash_id & 0x00ffffffU) == 0x00ffffffU)
        return photo_store_error("nor_identity", -ENODEV);
    if (start > UINT32_MAX || size == 0 || size > UINT32_MAX ||
        start + size > UINT32_MAX)
        return photo_store_error("partition_overflow", -ERANGE);
    if (start < flash->base || start - flash->base + size > (uint32_t)flash_bytes)
        return photo_store_error("partition_outside_nor", -ERANGE);
    return 0;
}

static int photo_store_recount_free(FATFS *fat)
{
    char drive[5];
    FATFS *scanned = NULL;
    DWORD cached;
    DWORD free_clusters = 0;
    FRESULT result;

    snprintf(drive, sizeof(drive), "%u:", (unsigned int)fat->pdrv);
#if FF_FS_REENTRANT
    if (!ff_req_grant(fat->sobj))
        return photo_store_error("fat_lock", -ETIMEDOUT);
#endif
    cached = fat->free_clst;
    fat->free_clst = UINT32_MAX;
    /* The RT-Thread volume mutex is recursive; keep invalidation and scan atomic. */
    result = f_getfree(drive, &free_clusters, &scanned);
    if (result != FR_OK || scanned != fat)
        fat->free_clst = cached;
#if FF_FS_REENTRANT
    ff_rel_grant(fat->sobj);
#endif
    if (result != FR_OK || scanned != fat)
    {
        LOG_E("step=fat_recount fatfs=%u", (unsigned int)result);
        return -EIO;
    }
    return 0;
}

static int photo_store_enter(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    if (!s_lock_initialized)
    {
        rt_mutex_init(&s_lock, "photo_fs", RT_IPC_FLAG_PRIO);
        s_lock_initialized = RT_TRUE;
    }
    rt_hw_interrupt_enable(level);
    return rt_mutex_take(&s_lock, RT_WAITING_FOREVER) == RT_EOK ? 0 : -EIO;
}

static int photo_store_leave(int result)
{
    s_last_error = result;
    rt_mutex_release(&s_lock);
    return result;
}

static int photo_store_parse_name(const char *name, const char *prefix,
                                  const char *suffix, uint32_t *id)
{
    const char *p;
    uint32_t value = 0;
    unsigned int digits = 0;
    size_t prefix_size = strlen(prefix);

    if (strncmp(name, prefix, prefix_size) != 0)
        return 0;
    p = name + prefix_size;
    while (*p >= '0' && *p <= '9')
    {
        unsigned int digit = (unsigned int)(*p - '0');

        if (++digits > 10U || value > (UINT32_MAX - digit) / 10U)
            return 0;
        value = value * 10U + digit;
        ++p;
    }
    if (digits == 0U || strcmp(p, suffix) != 0)
        return 0;
    *id = value;
    return 1;
}

static int photo_store_make_path(char *path, const char *name)
{
    int length = snprintf(path, PHOTO_STORE_PATH_MAX, "%s/%s",
                          PHOTO_STORE_DIRECTORY, name);

    return length >= 0 && (size_t)length < PHOTO_STORE_PATH_MAX ? 0 : -ENAMETOOLONG;
}

static int photo_store_compare(const photo_store_info_t *a,
                               const photo_store_info_t *b)
{
    if (a->id != b->id)
        return a->id > b->id ? 1 : -1;
    return strcmp(a->path, b->path);
}

/* A repeated bounded scan provides pagination without a file-count limit. */
static int photo_store_scan(const photo_store_info_t *before,
                             photo_store_info_t *next,
                             photo_store_info_t *oldest,
                             uint32_t *count, uint32_t *max_id)
{
    DIR *directory;
    struct dirent *entry;
    int result = 0;
    uint32_t found = 0;

    if (next != NULL)
        memset(next, 0, sizeof(*next));
    if (oldest != NULL)
        memset(oldest, 0, sizeof(*oldest));
    directory = opendir(PHOTO_STORE_DIRECTORY);
    if (directory == NULL)
    {
        result = photo_store_errno();
        if (result == -ENOENT)
        {
            if (count != NULL)
                *count = 0;
            return 0;
        }
        return photo_store_error("scan_open", result);
    }
    for (;;)
    {
        photo_store_info_t item;
        struct stat st;
        int is_photo;
        int is_temporary;

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
                result = photo_store_errno();
            break;
        }
        memset(&item, 0, sizeof(item));
        is_photo = photo_store_parse_name(entry->d_name, "photo_", ".jpg", &item.id);
        is_temporary = !is_photo &&
                       photo_store_parse_name(entry->d_name, ".photo_", ".tmp", &item.id);
        if (!is_photo && !is_temporary)
            continue;
        result = photo_store_make_path(item.path, entry->d_name);
        if (result < 0)
            break;
        if (stat(item.path, &st) < 0)
        {
            result = photo_store_errno();
            break;
        }
        if (!S_ISREG(st.st_mode))
            continue;
        if (max_id != NULL && item.id > *max_id)
            *max_id = item.id;
        if (!is_photo || st.st_size <= 0)
            continue;
        if ((uint64_t)st.st_size > UINT32_MAX || found == UINT32_MAX)
        {
            result = -EOVERFLOW;
            break;
        }
        item.size = (uint32_t)st.st_size;
        ++found;
        if (next != NULL &&
            (before == NULL || photo_store_compare(&item, before) < 0) &&
            (next->path[0] == '\0' || photo_store_compare(&item, next) > 0))
            *next = item;
        if (oldest != NULL &&
            (oldest->path[0] == '\0' || photo_store_compare(&item, oldest) < 0))
            *oldest = item;
    }
    if (closedir(directory) < 0 && result == 0)
        result = photo_store_errno();
    if (count != NULL)
        *count = found;
    return result;
}

/* Erased NOR must be blank throughout; a damaged first sector is insufficient. */
static int photo_store_is_erased(rt_device_t device,
                                 const struct rt_device_blk_geometry *geometry)
{
    uint8_t *buffer;
    uint32_t sector;
    int result = 1;

    buffer = rt_malloc(geometry->bytes_per_sector);
    if (buffer == NULL)
        return -ENOMEM;
    if (rt_device_open(device, RT_DEVICE_OFLAG_RDONLY) != RT_EOK)
    {
        rt_free(buffer);
        return -EIO;
    }
    for (sector = 0; sector < geometry->sector_count; ++sector)
    {
        uint32_t i;

        if (rt_device_read(device, sector, buffer, 1) != 1)
        {
            result = -EIO;
            break;
        }
        for (i = 0; i < geometry->bytes_per_sector; ++i)
        {
            if (buffer[i] != 0xffU)
            {
                result = 0;
                break;
            }
        }
        if (result != 1)
            break;
    }
    if (rt_device_close(device) != RT_EOK && result == 1)
        result = -EIO;
    rt_free(buffer);
    return result;
}

static int photo_store_cleanup_temporary(void)
{
    DIR *directory = opendir(PHOTO_STORE_DIRECTORY);
    int result = 0;

    if (directory == NULL)
    {
        result = photo_store_errno();
        return result == -ENOENT ? 0 : photo_store_error("temporary_open", result);
    }
    for (;;)
    {
        struct dirent *entry;
        uint32_t id;
        char path[PHOTO_STORE_PATH_MAX];
        struct stat st;

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
                result = photo_store_errno();
            break;
        }
        if (!photo_store_parse_name(entry->d_name, ".photo_", ".tmp", &id))
            continue;
        result = photo_store_make_path(path, entry->d_name);
        if (result < 0)
            break;
        if (stat(path, &st) < 0)
        {
            result = photo_store_errno();
            break;
        }
        if (S_ISREG(st.st_mode) && unlink(path) < 0)
        {
            result = photo_store_errno();
            break;
        }
    }
    if (closedir(directory) < 0 && result == 0)
        result = photo_store_errno();
    return result;
}

static int photo_store_init_locked(void)
{
    rt_device_t device;
    struct rt_device_blk_geometry geometry;
    struct rt_device_phy_addr_mapping mapping;
    struct dfs_filesystem *filesystem;
    const char *mounted_path;
    struct stat st;
    struct statfs space;
    int result;
    int mount_error;

    if (s_ready)
        return 0;
    result = photo_store_validate_region();
    if (result < 0)
        return result;
    if (!s_default_dma_configured)
    {
        const char *reason;

        result = photo_store_configure_dma_locked(&reason);
        if (result < 0)
        {
            LOG_E("default NOR DMA error=%d reason=%s", result, reason);
            return result;
        }
        s_default_dma_configured = RT_TRUE;
#if PHOTO_STORE_NOR_DMA_DETACH
        LOG_I("NOR write mode=CPU driven DMA engine detached");
#else
        LOG_I("NOR write mode=DMA priority=very high");
#endif
    }
    device = rt_device_find(PHOTO_STORE_DEVICE);
    if (device == RT_NULL)
    {
        register_mtd_device(FS_REGION_START_ADDR, FS_REGION_SIZE, PHOTO_STORE_DEVICE);
        device = rt_device_find(PHOTO_STORE_DEVICE);
    }
    if (device == RT_NULL)
        return photo_store_error("device_find", -ENODEV);
    memset(&geometry, 0, sizeof(geometry));
    if (rt_device_control(device, RT_DEVICE_CTRL_BLK_GETGEOME, &geometry) != RT_EOK ||
        geometry.bytes_per_sector == 0 || geometry.sector_count == 0 ||
        (uint64_t)geometry.bytes_per_sector * geometry.sector_count != FS_REGION_SIZE)
        return photo_store_error("device_geometry", -EINVAL);
    memset(&mapping, 0, sizeof(mapping));
    if (rt_device_control(device, RT_DEVICE_CTRL_GET_PHY_ADDR, &mapping) != RT_EOK ||
        mapping.physical_addr != FS_REGION_START_ADDR)
        return photo_store_error("device_mapping", -EINVAL);
    mounted_path = dfs_filesystem_get_mounted_path(device);
    if (mounted_path != NULL && strcmp(mounted_path, "/") != 0)
        return photo_store_error("device_mount_path", -EBUSY);
    filesystem = dfs_filesystem_lookup("/");
    if (filesystem != NULL && filesystem->dev_id != device)
        return photo_store_error("mount_owner", -EBUSY);
    if (mounted_path == NULL)
    {
        int layout_changed = 0;

        result = photo_store_signature_layout_changed(&layout_changed);
        if (result < 0)
            return photo_store_error("signature_read", result);
        errno = 0;
        if (dfs_mount(PHOTO_STORE_DEVICE, "/", "elm", 0, NULL) != 0)
        {
            mount_error = photo_store_posix_error("mount");
            if (layout_changed)
            {
                /* The build's FS region differs from the one that created this
                 * volume, so its superblock cannot be valid here: rebuild it. */
                LOG_I("fs region changed, rebuilding %s", PHOTO_STORE_DEVICE);
                if (dfs_mkfs("elm", PHOTO_STORE_DEVICE) != 0)
                    return photo_store_error("format_layout", -EIO);
                errno = 0;
                if (dfs_mount(PHOTO_STORE_DEVICE, "/", "elm", 0, NULL) != 0)
                    return photo_store_posix_error("mount_layout");
            }
            else
            {
                result = photo_store_is_erased(device, &geometry);
                if (result <= 0)
                    return photo_store_error(result < 0 ? "blank_read" : "mount_nonblank",
                                              result < 0 ? result : mount_error);
                if (dfs_mkfs("elm", PHOTO_STORE_DEVICE) != 0)
                    return photo_store_error("format_blank", -EIO);
                errno = 0;
                if (dfs_mount(PHOTO_STORE_DEVICE, "/", "elm", 0, NULL) != 0)
                    return photo_store_posix_error("mount_formatted");
            }
        }
        result = photo_store_signature_write();
        if (result < 0)
            return photo_store_error("signature_write", result);
    }
    filesystem = dfs_filesystem_lookup("/");
    if (filesystem == NULL || filesystem->ops == NULL ||
        strcmp(filesystem->ops->name, "elm") != 0 || filesystem->data == NULL)
        return photo_store_error("filesystem_type", -EINVAL);
    result = photo_store_recount_free((FATFS *)filesystem->data);
    if (result < 0)
        return result;
    if (statfs("/", &space) < 0)
        return photo_store_posix_error("statfs");
    s_cluster_bytes = (uint32_t)((FATFS *)filesystem->data)->csize *
                      (uint32_t)space.f_bsize;
    if (s_cluster_bytes == 0)
        return photo_store_error("cluster_size", -EINVAL);
    if (stat(PHOTO_STORE_DIRECTORY, &st) < 0)
    {
        result = photo_store_errno();
        if (result != -ENOENT)
            return photo_store_error("directory_stat", result);
        LOG_I("directory=%s absent, album empty", PHOTO_STORE_DIRECTORY);
    }
    else if (!S_ISDIR(st.st_mode))
        return photo_store_error("directory_type", -ENOTDIR);
    result = photo_store_scan(NULL, NULL, NULL, NULL, &s_last_id);
    if (result < 0)
        return photo_store_error("catalog_scan", result);
    s_ready = RT_TRUE;
    return 0;
}

static int photo_store_space(uint64_t *total, uint64_t *free_bytes)
{
    struct statfs space;

    if (statfs("/", &space) < 0)
        return photo_store_posix_error("space");
    if (space.f_bsize == 0 || space.f_bfree > space.f_blocks)
        return -EIO;
    *total = (uint64_t)space.f_blocks * space.f_bsize;
    *free_bytes = (uint64_t)space.f_bfree * space.f_bsize;
    return 0;
}

static int photo_store_delete_oldest(void)
{
    photo_store_info_t oldest;
    struct stat st;
    int result;

    /* Previous firmware kept this redundant copy. Reclaim it before originals. */
    if (stat(PHOTO_STORE_LEGACY_LATEST, &st) == 0)
    {
        if (S_ISREG(st.st_mode))
            return unlink(PHOTO_STORE_LEGACY_LATEST) == 0 ? 0 : photo_store_errno();
    }
    else
    {
        result = photo_store_errno();
        if (result != -ENOENT)
            return result;
    }
    result = photo_store_scan(NULL, NULL, &oldest, NULL, NULL);
    if (result < 0)
        return result;
    if (oldest.path[0] == '\0')
        return -ENOSPC;
    return unlink(oldest.path) == 0 ? 0 : photo_store_errno();
}

static int photo_store_reserve(size_t size)
{
    uint64_t required = ((uint64_t)size + s_cluster_bytes - 1U) /
                        s_cluster_bytes * s_cluster_bytes;
    uint64_t total;
    uint64_t free_bytes;
    int result;

    /* Leave clusters for directory growth during temporary creation and rename. */
    required += 2U * (uint64_t)s_cluster_bytes;
    for (;;)
    {
        result = photo_store_space(&total, &free_bytes);
        if (result < 0)
            return result;
        if (required > total)
            return -EFBIG;
        if (free_bytes >= required)
            return 0;
        result = photo_store_delete_oldest();
        if (result < 0)
            return result;
    }
}

static int photo_store_create_error(const char *step)
{
    int result = photo_store_errno();
    uint64_t total;
    uint64_t free_bytes;

    /* FatFs also maps allocation denial to EROFS; confirm exhaustion first. */
    if (result == -EROFS && photo_store_space(&total, &free_bytes) == 0 &&
        free_bytes == 0)
        result = -ENOSPC;
    return photo_store_error(step, result);
}

static int photo_store_ensure_directory(void)
{
    struct stat st;
    int result;

    if (stat(PHOTO_STORE_DIRECTORY, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : photo_store_error("directory_type", -ENOTDIR);
    result = photo_store_errno();
    if (result != -ENOENT)
        return photo_store_error("directory_stat", result);
    if (mkdir(PHOTO_STORE_DIRECTORY, 0777) < 0)
        return photo_store_create_error("directory_create");
    return 0;
}

static int photo_store_write_temporary(const char *path, const void *data, size_t size)
{
    size_t position = 0;
    int result = 0;
    int fd;
    int close_result;

    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
    {
        result = photo_store_create_error("temporary_create");
        return result;
    }
    while (position < size)
    {
        size_t chunk = size - position;
        int amount;

        if (chunk > PHOTO_STORE_IO_CHUNK)
            chunk = PHOTO_STORE_IO_CHUNK;
        amount = write(fd, (const uint8_t *)data + position, chunk);
        if (amount < 0)
        {
            result = photo_store_posix_error("temporary_write");
            break;
        }
        if ((size_t)amount < chunk)
        {
            result = photo_store_error("temporary_short_write", -ENOSPC);
            break;
        }
        position += (size_t)amount;
    }
    if (result == 0)
    {
        int flush_result;

        errno = 0;
        flush_result = fsync(fd);
        if (flush_result < 0)
            result = photo_store_error("temporary_sync",
                       flush_result < -1 ? flush_result : photo_store_errno());
    }
    close_result = close(fd);
    if (close_result < 0)
    {
        int close_error = photo_store_posix_error("temporary_close");
        if (result == 0)
            result = close_error;
    }
    if (result < 0 && unlink(path) < 0)
        photo_store_posix_error("temporary_remove");
    return result;
}

int photo_store_init(void)
{
    int result = photo_store_enter();

    if (result < 0)
        return result;
    return photo_store_leave(photo_store_init_locked());
}

int photo_store_get_status(photo_store_status_t *status)
{
    uint64_t total;
    uint64_t free_bytes;
    int result;

    if (status == NULL)
        return -EINVAL;
    memset(status, 0, sizeof(*status));
    result = photo_store_enter();
    if (result < 0)
        return result;
    result = photo_store_init_locked();
    status->ready = s_ready != RT_FALSE;
    if (result == 0)
        result = photo_store_scan(NULL, NULL, NULL, &status->photo_count, NULL);
    if (result == 0)
        result = photo_store_space(&total, &free_bytes);
    if (result == 0)
    {
        if (total > UINT32_MAX || free_bytes > UINT32_MAX)
            result = -EOVERFLOW;
        else
        {
            status->total_bytes = (uint32_t)total;
            status->free_bytes = (uint32_t)free_bytes;
        }
    }
    status->last_error = result == 0 ? s_last_error : result;
    return photo_store_leave(result);
}

int photo_store_list(uint32_t offset, photo_store_info_t *items, size_t capacity,
                     size_t *written, uint32_t *total)
{
    photo_store_info_t cursor;
    photo_store_info_t next;
    uint32_t count = 0;
    uint32_t index;
    size_t wanted;
    int result;

    if (written == NULL || total == NULL || (capacity != 0 && items == NULL))
        return -EINVAL;
    *written = 0;
    *total = 0;
    result = photo_store_enter();
    if (result < 0)
        return result;
    result = photo_store_init_locked();
    if (result == 0)
        result = photo_store_scan(NULL, NULL, NULL, &count, NULL);
    if (result < 0)
        return photo_store_leave(result);
    *total = count;
    if (offset >= count || capacity == 0)
        return photo_store_leave(0);
    wanted = count - offset;
    if (wanted > capacity)
        wanted = capacity;
    memset(&cursor, 0, sizeof(cursor));
    for (index = 0; index < offset + wanted; ++index)
    {
        result = photo_store_scan(index == 0 ? NULL : &cursor, &next, NULL, NULL, NULL);
        if (result < 0 || next.path[0] == '\0')
            return photo_store_leave(result < 0 ? result : -EIO);
        cursor = next;
        if (index >= offset)
            items[(*written)++] = next;
    }
    return photo_store_leave(0);
}

int photo_store_save_jpeg(const void *data, size_t size, photo_store_info_t *info)
{
    photo_store_info_t saved;
    char temporary[PHOTO_STORE_PATH_MAX];
    uint32_t max_id;
    int result;
    const uint8_t *bytes = data;

    if (info == NULL || data == NULL || size < 4 || size > UINT32_MAX ||
        bytes[0] != 0xffU || bytes[1] != 0xd8U ||
        bytes[size - 2U] != 0xffU || bytes[size - 1U] != 0xd9U)
        return -EINVAL;
    memset(info, 0, sizeof(*info));
    result = photo_store_enter();
    if (result < 0)
        return result;
    result = photo_store_init_locked();
    if (result == 0)
        result = photo_store_cleanup_temporary();
    max_id = s_last_id;
    if (result == 0)
        result = photo_store_scan(NULL, NULL, NULL, NULL, &max_id);
    if (result < 0)
        goto finished;
    if (max_id == UINT32_MAX)
    {
        result = -EOVERFLOW;
        goto finished;
    }
    memset(&saved, 0, sizeof(saved));
    saved.id = max_id + 1U;
    saved.size = (uint32_t)size;
    snprintf(saved.path, sizeof(saved.path), PHOTO_STORE_DIRECTORY "/photo_%010lu.jpg",
             (unsigned long)saved.id);
    snprintf(temporary, sizeof(temporary), PHOTO_STORE_DIRECTORY "/.photo_%010lu.tmp",
             (unsigned long)saved.id);
    s_last_id = saved.id;
    for (;;)
    {
        result = photo_store_reserve(size);
        if (result < 0)
        {
            photo_store_error("reserve", result);
            break;
        }
        result = photo_store_ensure_directory();
        if (result < 0)
            break;
        result = photo_store_write_temporary(temporary, data, size);
        if (result == 0)
        {
            if (rename(temporary, saved.path) == 0)
            {
                *info = saved;
                break;
            }
            result = photo_store_posix_error("rename");
            if (unlink(temporary) < 0)
                photo_store_posix_error("temporary_remove");
        }
        if (result != -ENOSPC)
            break;
        result = photo_store_delete_oldest();
        if (result < 0)
            break;
    }
finished:
    return photo_store_leave(result);
}

int photo_store_read_jpeg(const char *path, void *buffer, size_t capacity, size_t *size)
{
    struct stat st;
    uint32_t id;
    size_t position = 0;
    int fd;
    int result;
    const size_t prefix_size = sizeof(PHOTO_STORE_DIRECTORY "/") - 1U;

    if (path == NULL || size == NULL ||
        (buffer == NULL && capacity != 0) ||
        strncmp(path, PHOTO_STORE_DIRECTORY "/", prefix_size) != 0 ||
        !photo_store_parse_name(path + prefix_size, "photo_", ".jpg", &id))
        return -EINVAL;
    *size = 0;
    result = photo_store_enter();
    if (result < 0)
        return result;
    result = photo_store_init_locked();
    if (result < 0)
        return photo_store_leave(result);
    if (stat(path, &st) < 0)
        return photo_store_leave(photo_store_errno());
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX)
        return photo_store_leave(-EINVAL);
    *size = (size_t)st.st_size;
    if (buffer == NULL && capacity == 0)
        return photo_store_leave(0);
    if (capacity < *size)
        return photo_store_leave(-ENOBUFS);
    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return photo_store_leave(photo_store_errno());
    result = 0;
    while (position < *size)
    {
        size_t chunk = *size - position;
        int amount;

        if (chunk > PHOTO_STORE_IO_CHUNK)
            chunk = PHOTO_STORE_IO_CHUNK;
        amount = read(fd, (uint8_t *)buffer + position, chunk);
        if (amount <= 0)
        {
            result = amount == 0 ? -EIO : photo_store_errno();
            break;
        }
        position += (size_t)amount;
    }
    if (close(fd) < 0 && result == 0)
        result = photo_store_errno();
    return photo_store_leave(result);
}

int photo_store_delete_at(uint32_t offset, photo_store_info_t *deleted)
{
    photo_store_info_t cursor;
    photo_store_info_t next;
    struct stat st;
    uint32_t index;
    int result;

    if (deleted != NULL)
        memset(deleted, 0, sizeof(*deleted));
    /* Walk to the requested item and unlink it under one lock, so a concurrent
     * capture cannot insert a photo between the lookup and the removal. */
    result = photo_store_enter();
    if (result < 0)
        return result;
    result = photo_store_init_locked();
    if (result < 0)
        return photo_store_leave(result);
    memset(&cursor, 0, sizeof(cursor));
    for (index = 0; index <= offset; ++index)
    {
        result = photo_store_scan(index == 0U ? NULL : &cursor, &next, NULL, NULL,
                                  NULL);
        if (result < 0)
            return photo_store_leave(result);
        if (next.path[0] == '\0')
            return photo_store_leave(-ENOENT);
        cursor = next;
    }
    if (stat(cursor.path, &st) < 0)
        return photo_store_leave(photo_store_posix_error("delete_stat"));
    if (!S_ISREG(st.st_mode))
        return photo_store_leave(-EINVAL);
    if (unlink(cursor.path) < 0)
        return photo_store_leave(photo_store_posix_error("delete_unlink"));
    if (deleted != NULL)
        *deleted = cursor;
    return photo_store_leave(0);
}

#define PHOTO_DELETE_LIST_MAX 6U

static void photo_delete_command(int argc, char **argv)
{
    photo_store_info_t items[PHOTO_DELETE_LIST_MAX];
    photo_store_info_t deleted;
    photo_store_status_t status;
    size_t written = 0;
    uint32_t total = 0;
    uint32_t index;
    int result;

    if (argc == 1)
    {
        result = photo_store_list(0U, items, PHOTO_DELETE_LIST_MAX, &written,
                                  &total);
        if (result < 0)
        {
            rt_kprintf("photo: list failed %d\n", result);
            return;
        }
        rt_kprintf("photo: total=%u list=%u newest first\n",
                   (unsigned int)total, (unsigned int)written);
        for (index = 0; index < written; ++index)
            rt_kprintf("photo: %u %s %u\n", (unsigned int)index,
                       items[index].path, (unsigned int)items[index].size);
        return;
    }
    if (argc != 2)
    {
        rt_kprintf("usage: photo_delete [index]\n");
        return;
    }
    index = (uint32_t)strtoul(argv[1], NULL, 10);
    result = photo_store_delete_at(index, &deleted);
    if (result < 0)
    {
        rt_kprintf("photo: delete %u failed %d\n", (unsigned int)index, result);
        return;
    }
    rt_kprintf("photo: deleted %s\n", deleted.path);
    if (photo_store_get_status(&status) == 0)
        rt_kprintf("photo: %u left\n", (unsigned int)status.photo_count);
}
MSH_CMD_EXPORT_ALIAS(photo_delete_command, photo_delete,
                     Album photo delete by index and album listing);

#define PHOTO_STORE_ROTATION_BYTES 16U
#define PHOTO_STORE_ROTATION_TEMP PHOTO_STORE_DIRECTORY "/.rotation.tmp"

static const char *const s_rotation_paths[2] =
{
    PHOTO_STORE_DIRECTORY "/.rotation0.cfg",
    PHOTO_STORE_DIRECTORY "/.rotation1.cfg",
};

typedef struct
{
    uint32_t sequence;
    uint16_t degrees;
    int exists;
    int valid;
} photo_store_rotation_t;

static uint32_t photo_store_rotation_crc(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned int bit;

    for (i = 0U; i < size; ++i)
    {
        crc ^= bytes[i];
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ UINT32_MAX;
}

static uint32_t photo_store_rotation_get32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void photo_store_rotation_put32(uint8_t *bytes, uint32_t value)
{
    unsigned int i;

    for (i = 0U; i < 4U; ++i)
        bytes[i] = (uint8_t)(value >> (i * 8U));
}

/* Invalid content is recoverable from the other slot; I/O errors are not hidden. */
static int photo_store_rotation_read(unsigned int slot, photo_store_rotation_t *record)
{
    uint8_t bytes[PHOTO_STORE_ROTATION_BYTES];
    struct stat st;
    size_t position = 0U;
    int result = 0;
    int fd;

    memset(record, 0, sizeof(*record));
    if (stat(s_rotation_paths[slot], &st) < 0)
    {
        result = photo_store_errno();
        return result == -ENOENT ? 0 : result;
    }
    record->exists = 1;
    if (!S_ISREG(st.st_mode) || st.st_size != PHOTO_STORE_ROTATION_BYTES)
        return 0;
    fd = open(s_rotation_paths[slot], O_RDONLY, 0);
    if (fd < 0)
        return photo_store_errno();
    while (position < sizeof(bytes))
    {
        int amount = read(fd, bytes + position, sizeof(bytes) - position);

        if (amount <= 0)
        {
            result = amount == 0 ? 0 : photo_store_errno();
            break;
        }
        position += (size_t)amount;
    }
    if (close(fd) < 0 && result == 0)
        result = photo_store_errno();
    if (result < 0 || position != sizeof(bytes))
        return result;
    record->degrees = (uint16_t)((uint16_t)bytes[6] | ((uint16_t)bytes[7] << 8));
    if (memcmp(bytes, "CROT", 4U) != 0 || bytes[4] != 1U || bytes[5] != 0U ||
        (record->degrees != 0U && record->degrees != 180U) ||
        photo_store_rotation_get32(bytes + 12U) != photo_store_rotation_crc(bytes, 12U))
        return 0;
    record->sequence = photo_store_rotation_get32(bytes + 8U);
    record->valid = 1;
    return 0;
}

static int photo_store_rotation_latest(const photo_store_rotation_t records[2])
{
    uint32_t difference;

    if (!records[0].valid)
        return records[1].valid ? 1 : -1;
    if (!records[1].valid)
        return 0;
    /* Adjacent generations remain ordered across uint32 wraparound. */
    difference = records[1].sequence - records[0].sequence;
    return difference != 0U && difference < 0x80000000U ? 1 : 0;
}

static int photo_store_rotation_remove(const char *path)
{
    int result;

    if (unlink(path) == 0)
        return 0;
    result = photo_store_errno();
    return result == -ENOENT ? 0 : result;
}

int photo_store_load_rotation(uint16_t *degrees)
{
    photo_store_rotation_t records[2];
    int result;
    int latest;

    if (degrees == NULL)
        return -EINVAL;
    *degrees = 0U;
    result = photo_store_enter();
    if (result < 0)
        return result;
    result = photo_store_init_locked();
    if (result == 0)
        result = photo_store_rotation_read(0U, &records[0]);
    if (result == 0)
        result = photo_store_rotation_read(1U, &records[1]);
    if (result == 0)
    {
        latest = photo_store_rotation_latest(records);
        if (latest >= 0)
            *degrees = records[latest].degrees;
        else if (records[0].exists || records[1].exists)
            result = -EIO;
    }
    return photo_store_leave(result);
}

int photo_store_save_rotation(uint16_t degrees)
{
    photo_store_rotation_t records[2];
    uint8_t bytes[PHOTO_STORE_ROTATION_BYTES] = {'C', 'R', 'O', 'T', 1U, 0U};
    uint32_t sequence;
    unsigned int target;
    int latest;
    int result;

    if (degrees != 0U && degrees != 180U)
        return -EINVAL;
    result = photo_store_enter();
    if (result < 0)
        return result;
    result = photo_store_init_locked();
    if (result == 0)
        result = photo_store_rotation_read(0U, &records[0]);
    if (result == 0)
        result = photo_store_rotation_read(1U, &records[1]);
    if (result < 0)
        return photo_store_leave(result);
    latest = photo_store_rotation_latest(records);
    if (latest >= 0 && records[latest].degrees == degrees)
        return photo_store_leave(0);
    target = latest >= 0 ? (unsigned int)(latest ^ 1) : 0U;
    sequence = latest >= 0 ? records[latest].sequence + 1U : 1U;
    bytes[6] = (uint8_t)degrees;
    bytes[7] = (uint8_t)(degrees >> 8);
    photo_store_rotation_put32(bytes + 8U, sequence);
    photo_store_rotation_put32(bytes + 12U, photo_store_rotation_crc(bytes, 12U));
    result = photo_store_rotation_remove(PHOTO_STORE_ROTATION_TEMP);
    if (result < 0)
        return photo_store_leave(result);
    for (;;)
    {
        result = photo_store_reserve(sizeof(bytes));
        if (result == 0)
            result = photo_store_ensure_directory();
        if (result == 0)
            result = photo_store_write_temporary(PHOTO_STORE_ROTATION_TEMP, bytes, sizeof(bytes));
        if (result == 0)
        {
            /* Remove only the older/invalid slot, after the new data is durable.
             * The latest valid slot survives every step; rename never replaces. */
            result = photo_store_rotation_remove(s_rotation_paths[target]);
            if (result == 0 && rename(PHOTO_STORE_ROTATION_TEMP, s_rotation_paths[target]) < 0)
                result = photo_store_errno();
        }
        if (result == 0)
            break;
        (void)photo_store_rotation_remove(PHOTO_STORE_ROTATION_TEMP);
        if (result != -ENOSPC)
            break;
        result = photo_store_delete_oldest();
        if (result < 0)
            break;
    }
    return photo_store_leave(result);
}


#define PHOTO_STORE_SIGNATURE_BYTES 24U
#define PHOTO_STORE_SIGNATURE_VERSION 1U

typedef struct
{
    uint32_t start;
    uint32_t size;
    uint32_t generation;
    int valid;
} photo_store_signature_t;

/* Invalid content only means "unknown layout"; a read error is reported. */
static int photo_store_signature_read(photo_store_signature_t *record)
{
    uint8_t bytes[PHOTO_STORE_SIGNATURE_BYTES];
    int amount;

    memset(record, 0, sizeof(*record));
    amount = rt_flash_read(FS_META_REGION_START_ADDR, bytes, (int)sizeof(bytes));
    if (amount < 0)
        return -EIO;
    if ((size_t)amount != sizeof(bytes))
        return 0;
    if (memcmp(bytes, "CPFS", 4U) != 0 || bytes[4] != PHOTO_STORE_SIGNATURE_VERSION ||
        bytes[5] != 0U || bytes[6] != 0U || bytes[7] != 0U)
        return 0;
    if (photo_store_rotation_get32(bytes + 20U) !=
        photo_store_rotation_crc(bytes, 20U))
        return 0;
    record->start = photo_store_rotation_get32(bytes + 8U);
    record->size = photo_store_rotation_get32(bytes + 12U);
    record->generation = photo_store_rotation_get32(bytes + 16U);
    record->valid = 1;
    return 0;
}

/* A missing record is treated as "unknown layout" as well: it only decides
 * whether formatting is allowed after a failed mount, never before one. */
static int photo_store_signature_layout_changed(int *changed)
{
    photo_store_signature_t record;
    int result;

    *changed = 1;
    result = photo_store_signature_read(&record);
    if (result < 0)
        return result;
    if (record.valid && record.start == (uint32_t)FS_REGION_START_ADDR &&
        record.size == (uint32_t)FS_REGION_SIZE)
        *changed = 0;
    return 0;
}

/* Rewrites the record only when it does not already describe this layout, so a
 * normal boot costs no erase cycle. */
static int photo_store_signature_write(void)
{
    photo_store_signature_t record;
    uint8_t bytes[PHOTO_STORE_SIGNATURE_BYTES] = {'C', 'P', 'F', 'S',
                                                  PHOTO_STORE_SIGNATURE_VERSION, 0U, 0U, 0U};
    uint32_t generation = 1U;
    int result;

    result = photo_store_signature_read(&record);
    if (result < 0)
        return result;
    if (record.valid && record.start == (uint32_t)FS_REGION_START_ADDR &&
        record.size == (uint32_t)FS_REGION_SIZE)
        return 0;
    if (record.valid)
        generation = record.generation + 1U;
    photo_store_rotation_put32(bytes + 8U, (uint32_t)FS_REGION_START_ADDR);
    photo_store_rotation_put32(bytes + 12U, (uint32_t)FS_REGION_SIZE);
    photo_store_rotation_put32(bytes + 16U, generation);
    photo_store_rotation_put32(bytes + 20U, photo_store_rotation_crc(bytes, 20U));
    if (rt_flash_erase(FS_META_REGION_START_ADDR, (int)FS_META_REGION_SIZE) != RT_EOK)
        return -EIO;
    if (rt_flash_write(FS_META_REGION_START_ADDR, bytes, (int)sizeof(bytes)) !=
        (int)sizeof(bytes))
        return -EIO;
    LOG_I("volume signature generation=%u start=0x%08x size=0x%08x",
          (unsigned int)generation, (unsigned int)FS_REGION_START_ADDR,
          (unsigned int)FS_REGION_SIZE);
    return 0;
}
