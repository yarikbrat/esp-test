#pragma once
// Кільцевий журнал історії барометрів у NOR flash. Не потокобезпечний: власник впорядковує доступ.
// Запис seq лежить у слоті seq % capacity. Сектор стирається, коли пишеться його перший слот,
// тож кожен сектор стирається раз за коло (рівномірний знос). Дописування у стерту flash не зношує,
// тому кожен запис пишеться одразу; при втраті живлення гине щонайбільше запис, що пишеться.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HL_CHANNELS  2
#define HL_SEQ_EMPTY 0xFFFFFFFFu

// біти flags[ch]
#define HL_F_VALID 0x01u // тиск дійсний
#define HL_F_ZERO  0x02u // нуль задано, висота визначена
#define HL_F_CLEAR 0x04u // flags[0] маркера очищення: цей запис і все старіше очищено
#define HL_F_STATUS(f) (((f) >> 4) & 0x0Fu)
#define HL_F_MAKE(valid, zero, status) \
    (uint8_t)(((valid) ? HL_F_VALID : 0u) | ((zero) ? HL_F_ZERO : 0u) | (((status) & 0x0Fu) << 4))

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint16_t session;           // лічильник завантажень
    uint8_t flags[HL_CHANNELS]; // 0 = BMP585, 1 = BMP581
    uint32_t t_s;               // секунди від початку сесії
    float p_pa[HL_CHANNELS];    // NAN, якщо недійсний
    float h_m[HL_CHANNELS];     // NAN, якщо немає нуля або тиск недійсний
    uint32_t crc;               // CRC32 усіх попередніх полів
} hl_record_t;

_Static_assert(sizeof(hl_record_t) == 32, "hl_record_t must stay 32 bytes");

// Бекенд flash: esp_partition на платі, емулятор у RAM на хості. Функції повертають 0 при успіху.
typedef struct {
    void *ctx;
    uint32_t sector_size;
    int (*read)(void *ctx, uint32_t off, void *dst, uint32_t len);
    int (*write)(void *ctx, uint32_t off, const void *src, uint32_t len);
    int (*erase_sector)(void *ctx, uint32_t off);
} hl_flash_t;

typedef struct {
    const hl_flash_t *fl;
    uint32_t n_sectors;
    uint32_t recs_per_sector;
    uint32_t capacity;   // записів у кільці
    uint32_t next_seq;
    uint32_t oldest_seq; // == next_seq, коли порожньо
    uint32_t write_errors;
    uint32_t corrupt;    // пошкоджені слоти, знайдені останнім скануванням
    int last_error;      // код останньої помилки flash, 0 якщо немає
} histlog_t;

// Відкриває журнал на n_sectors і шукає в ньому найновіший запис.
int histlog_open(histlog_t *hl, const hl_flash_t *fl, uint32_t n_sectors);

// Дописує запис; заповнює rec->seq і rec->crc.
int histlog_append(histlog_t *hl, hl_record_t *rec);

// Логічне очищення дописуванням маркера: без масового стирання, тож flash вільна для інших задач.
int histlog_clear(histlog_t *hl);

// Стирає всі сектори (секунди простою flash): лише для форматування при старті, до запуску задач.
int histlog_format(histlog_t *hl);

uint32_t histlog_count(const histlog_t *hl);

// Читає до max записів із seq < before_seq, найновіші першими (HL_SEQ_EMPTY = від найновішого).
// *next_before (необов'язковий) — курсор наступної сторінки, HL_SEQ_EMPTY, коли старішого нема;
// він береться з самого обходу, тож низка пропусків не обриває сторінки завчасно.
size_t histlog_read_recent(histlog_t *hl, uint32_t before_seq, hl_record_t *out, size_t max,
                           uint32_t *next_before);

#ifdef __cplusplus
}
#endif
