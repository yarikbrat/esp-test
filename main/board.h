#pragma once
// Pin map of the bench. The physical wiring is recorded in docs/connections.md; keep both in sync.

#define PIN_BRIDGE_TX 17 // -> module RXD
#define PIN_BRIDGE_RX 16 // <- module TXD
#define PIN_MOD_RST   25 // -> module RST, active low
#define PIN_MOD_DEF   26 // -> module DEF, active low; not wired at the moment

#define PIN_I2C0_SDA 21 // BMP585
#define PIN_I2C0_SCL 22
#define PIN_I2C1_SDA 18 // BMP581
#define PIN_I2C1_SCL 19

#define PIN_SONAR_TRIG 33
#define PIN_SONAR_ECHO 32 // through the 1k/2k divider when the sensor runs on 5 V

#define PIN_BOOT_BTN 0 // BOOT button, low when pressed

#define BRIDGE_UART_NUM 2
#define BRIDGE_BAUD     115200
#define HTTP_PORT       80
