#pragma once
// BMP581/BMP585 over I2C, one sensor per bus. Normal mode, 25 Hz, pressure x8, temperature x1.
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "stand.h"

#define BMP581_CHIP_ID 0x50
#define BMP585_CHIP_ID 0x51

typedef struct {
    const char *name;
    i2c_port_num_t port;
    int sda;
    int scl;
    uint8_t expect_id;
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool ready;
    uint8_t fail_streak;
    uint32_t next_init_ms;
} bmp58x_t;

bool bmp58x_bus_init(bmp58x_t *b);
// Reads one pressure sample, (re)initialising the sensor when needed. *p_pa is set only on SS_OK.
sensor_status_t bmp58x_poll(bmp58x_t *b, uint32_t now_ms, float *p_pa);
