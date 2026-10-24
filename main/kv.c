#include "kv.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "kv";
static nvs_handle_t s_nvs;

bool kv_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable (%s), erasing: settings fall back to defaults",
                 esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        err = nvs_open("stand", NVS_READWRITE, &s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

size_t kv_get(const char *key, uint8_t *buf, size_t cap)
{
    size_t len = cap;
    if (!s_nvs || nvs_get_blob(s_nvs, key, buf, &len) != ESP_OK) {
        return 0;
    }
    return len;
}

bool kv_set(const char *key, const uint8_t *buf, size_t len)
{
    if (!s_nvs) {
        return false;
    }
    esp_err_t err = nvs_set_blob(s_nvs, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool kv_erase(const char *key)
{
    if (!s_nvs) {
        return false;
    }
    esp_err_t err = nvs_erase_key(s_nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}
