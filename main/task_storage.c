// Задача storage: записує нулі барометрів у NVS за запитами з черги persist_q.
#include "app.h"
#include "esp_log.h"
#include "kv.h"

static const char *TAG = "storage";

// Єдине місце, що записує blob-и нулів. Воно зберігає значення каналу з RAM на момент запису, тож пізніше скидання
// ніколи не буде перезаписане старішим результатом калібрування.
void task_storage(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t ch;
        xQueueReceive(g_app.persist_q, &ch, portMAX_DELAY);
        app_lock();
        zero_cfg_t z = g_app.stand.baro[ch].zero;
        app_unlock();
        uint8_t buf[ST_ZERO_BLOB_SIZE];
        size_t n = st_encode_zero(&z, ch, buf, sizeof(buf));
        bool ok = n && kv_set(app_zero_key(ch), buf, n);
        app_lock();
        stand_zero_persisted(&g_app.stand, ch, ok);
        app_unlock();
        ESP_LOGI(TAG, "zero%u %s, persist %s", ch, z.set ? "set" : "cleared", ok ? "ok" : "FAILED");
    }
}
