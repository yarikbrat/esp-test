#include "bmp58x.h"

#include "altitude.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Register map as in Bosch BMP5_SensorAPI bmp5_defs.h.
#define REG_CHIP_ID     0x01
#define REG_TEMP_XLSB   0x1D
#define REG_INT_STATUS  0x27
#define REG_OSR_CONFIG  0x36
#define REG_ODR_CONFIG  0x37
#define REG_CMD         0x7E
#define CMD_SOFT_RESET  0xB6
#define OSR_PRESS_EN    0x40
#define OSR_P_8X        (0x03 << 3)
#define OSR_T_1X        0x00
#define ODR_DEEP_DIS    0x80
#define ODR_25HZ        (0x14 << 2)
#define MODE_NORMAL     0x01

#define I2C_TIMEOUT_MS  20
#define REINIT_PERIOD_MS 1000
#define FAILS_BEFORE_REINIT 3

static const char *TAG = "bmp58x";

static esp_err_t reg_write(bmp58x_t *b, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(b->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(bmp58x_t *b, uint8_t reg, uint8_t *dst, size_t len)
{
    return i2c_master_transmit_receive(b->dev, &reg, 1, dst, len, I2C_TIMEOUT_MS);
}

bool bmp58x_bus_init(bmp58x_t *b)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = b->port,
        .sda_io_num = b->sda,
        .scl_io_num = b->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, // the sensor boards also carry their own pull-ups
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &b->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: bus init failed: %s", b->name, esp_err_to_name(err));
        return false;
    }
    return true;
}

static void drop_device(bmp58x_t *b)
{
    if (b->dev) {
        i2c_master_bus_rm_device(b->dev);
        b->dev = NULL;
    }
    b->ready = false;
}

static sensor_status_t sensor_init(bmp58x_t *b)
{
    drop_device(b);
    i2c_master_bus_reset(b->bus); // frees SDA if a sensor was cut off mid-transfer
    uint16_t addr = 0;
    for (uint16_t a = 0x47; a >= 0x46 && !addr; a--) {
        if (i2c_master_probe(b->bus, a, I2C_TIMEOUT_MS) == ESP_OK) {
            addr = a;
        }
    }
    if (!addr) {
        return SS_NOT_FOUND;
    }
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(b->bus, &dcfg, &b->dev) != ESP_OK) {
        return SS_COMM_ERROR;
    }
    uint8_t id = 0;
    if (reg_read(b, REG_CHIP_ID, &id, 1) != ESP_OK) {
        drop_device(b);
        return SS_COMM_ERROR;
    }
    if (id != b->expect_id) {
        ESP_LOGW(TAG, "%s: chip id 0x%02x at 0x%02x, expected 0x%02x (sensors swapped?)", b->name, id, addr,
                 b->expect_id);
        drop_device(b);
        return SS_NOT_FOUND;
    }

    reg_write(b, REG_CMD, CMD_SOFT_RESET); // NACK here is normal: the chip resets mid-transfer
    vTaskDelay(pdMS_TO_TICKS(10));
    // After a NACK the driver stays in an error state until the bus is reset.
    esp_err_t err = ESP_FAIL;
    uint8_t v;
    for (int i = 0; i < 5 && err != ESP_OK; i++) {
        i2c_master_bus_reset(b->bus);
        err = reg_read(b, REG_INT_STATUS, &v, 1); // clears the power-on flag
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    if (err == ESP_OK) {
        err = reg_write(b, REG_ODR_CONFIG, ODR_DEEP_DIS); // standby before changing the config
    }
    vTaskDelay(pdMS_TO_TICKS(3));
    if (err == ESP_OK) {
        err = reg_write(b, REG_OSR_CONFIG, OSR_PRESS_EN | OSR_P_8X | OSR_T_1X);
    }
    if (err == ESP_OK) {
        err = reg_write(b, REG_ODR_CONFIG, ODR_DEEP_DIS | ODR_25HZ | MODE_NORMAL);
    }
    if (err != ESP_OK) {
        drop_device(b);
        return SS_COMM_ERROR;
    }
    b->ready = true;
    b->fail_streak = 0;
    ESP_LOGI(TAG, "%s ready at 0x%02x", b->name, addr);
    return SS_REINIT; // first sample comes on the next poll
}

sensor_status_t bmp58x_poll(bmp58x_t *b, uint32_t now_ms, float *p_pa)
{
    if (!b->bus) {
        return SS_NOT_FOUND;
    }
    if (!b->ready) {
        if ((int32_t)(now_ms - b->next_init_ms) < 0) {
            return SS_NOT_FOUND;
        }
        b->next_init_ms = now_ms + REINIT_PERIOD_MS;
        return sensor_init(b);
    }
    uint8_t d[6];
    if (reg_read(b, REG_TEMP_XLSB, d, sizeof(d)) != ESP_OK) {
        if (++b->fail_streak >= FAILS_BEFORE_REINIT) {
            ESP_LOGW(TAG, "%s: %u read errors in a row, re-initialising", b->name, b->fail_streak);
            drop_device(b);
            b->next_init_ms = now_ms;
        }
        return SS_COMM_ERROR;
    }
    b->fail_streak = 0;
    uint32_t raw = ((uint32_t)d[5] << 16) | ((uint32_t)d[4] << 8) | d[3];
    float p = (float)raw / 64.0f;
    if (!baro_pressure_plausible(p)) {
        return SS_OUT_OF_RANGE;
    }
    *p_pa = p;
    return SS_OK;
}
