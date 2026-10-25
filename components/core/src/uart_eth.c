#include "uart_eth.h"

#include <stdio.h>

size_t uart_eth_form(const netcfg_t *cfg, char *buf, size_t cap)
{
    char ip[16], mask[16], gw[16];
    netcfg_format_ipv4(cfg->ip, ip, sizeof(ip));
    netcfg_format_ipv4(cfg->mask, mask, sizeof(mask));
    netcfg_format_ipv4(cfg->gw, gw, sizeof(gw));
    // Порядок полів і фіксовані значення як у тому, що надсилає сама сторінка конфігуратора.
    int n = snprintf(buf, cap,
                     "dvn=WSDEV0001&lca=%s&lcp=%u&wbp=%u&wkm=0&lcm=%s&lcg=%s&dsa=192.168.1.3&dsp=4196"
                     "&ipm=0&bdr=B&dtb=0&prt=0&stb=0&flc=0&ndr=0&nrt=300&rct=12&ptc=0&rto=0&emh=0&rcg=0"
                     "&nwk=&rtp=&post=Submit",
                     ip, (unsigned)UART_ETH_DATA_PORT, (unsigned)UART_ETH_WEB_PORT, mask, gw);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
