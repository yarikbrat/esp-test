#pragma once
// Карта виводів стенда. Фізичне підключення описане в docs/connections.md; тримайте обидва файли узгодженими.

#define PIN_BRIDGE_TX 17 // -> RXD модуля
#define PIN_BRIDGE_RX 16 // <- TXD модуля
#define PIN_MOD_RST   25 // -> RST модуля, активний низький рівень
#define PIN_MOD_DEF   26 // -> DEF модуля, активний низький рівень; зараз не під'єднаний

#define PIN_I2C0_SDA 21 // BMP585
#define PIN_I2C0_SCL 22
#define PIN_I2C1_SDA 18 // BMP581
#define PIN_I2C1_SCL 19

#define PIN_SONAR_TRIG 33
#define PIN_SONAR_ECHO 32 // через дільник 1k/2k, коли датчик живиться від 5 V

#define PIN_BOOT_BTN 0 // кнопка BOOT, низький рівень при натисканні

#define BRIDGE_UART_NUM 2
#define BRIDGE_BAUD     115200
#define HTTP_PORT       80
