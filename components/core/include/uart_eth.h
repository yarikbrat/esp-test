#pragma once
// Settings form of the Waveshare UART TO ETH (B) web configurator (firmware V1.452).
// See docs/uart-to-eth-b.md for the field list and the port rule.
#include <stddef.h>

#include "netcfg.h"

#define UART_ETH_DATA_PORT 80   // TCP server port, carries HTTP to the ESP32
#define UART_ETH_WEB_PORT  8000 // the module's own configurator, moved off port 80

// Full urlencoded body for POST /ok.html: the given address, TCP Server on UART_ETH_DATA_PORT, configurator
// on UART_ETH_WEB_PORT, 115200 8N1, no flow control, no-data restart off. Returns length, 0 if it does not fit.
size_t uart_eth_form(const netcfg_t *cfg, char *buf, size_t cap);
