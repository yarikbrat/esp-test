#include "flash_part.h"

#include "esp_log.h"
#include "esp_partition.h"

#define HISTLOG_SUBTYPE 0x40
#define SECTOR_SIZE     4096

static const char *TAG = "flash_part";

static int part_read(void *ctx, uint32_t off, void *dst, uint32_t len)
{
    return esp_partition_read(ctx, off, dst, len);
}

static int part_write(void *ctx, uint32_t off, const void *src, uint32_t len)
{
    return esp_partition_write(ctx, off, src, len);
}

static int part_erase(void *ctx, uint32_t off)
{
    return esp_partition_erase_range(ctx, off, SECTOR_SIZE);
}

uint32_t flash_part_open(hl_flash_t *ops)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, HISTLOG_SUBTYPE, "histlog");
    if (!p) {
        ESP_LOGE(TAG, "histlog partition not found");
        return 0;
    }
    *ops = (hl_flash_t){
        .ctx = (void *)p,
        .sector_size = SECTOR_SIZE,
        .read = part_read,
        .write = part_write,
        .erase_sector = part_erase,
    };
    return p->size / SECTOR_SIZE;
}
