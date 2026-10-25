#pragma once
// Форма налаштувань веб-конфігуратора Waveshare UART TO ETH (B) (прошивка V1.452).
// Список полів і правило портів див. у docs/uart-to-eth-b.md.
#include <stddef.h>

#include "netcfg.h"

#define UART_ETH_DATA_PORT 80   // порт TCP-сервера, несе HTTP до ESP32
#define UART_ETH_WEB_PORT  8000 // власний конфігуратор модуля, перенесений з порту 80

// Повне urlencoded-тіло для POST /ok.html: задана адреса, TCP Server на UART_ETH_DATA_PORT, конфігуратор
// на UART_ETH_WEB_PORT, 115200 8N1, без керування потоком, рестарт без даних вимкнено. Повертає довжину, 0 якщо не вміщається.
size_t uart_eth_form(const netcfg_t *cfg, char *buf, size_t cap);
