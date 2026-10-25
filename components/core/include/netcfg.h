#pragma once
// Мережеві налаштування: розбір і перевірка IP, маски і шлюзу.
// Адреси — uint32_t у порядку хоста: "a.b.c.d" == (a<<24)|(b<<16)|(c<<8)|d.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETCFG_DEFAULT_IP   0xC0A89024u // 192.168.144.36
#define NETCFG_DEFAULT_MASK 0xFFFFFF00u // 255.255.255.0
#define NETCFG_DEFAULT_GW   0x00000000u // без шлюзу

typedef struct {
    uint32_t ip;
    uint32_t mask;
    uint32_t gw; // 0 = без шлюзу
} netcfg_t;

typedef enum {
    NETCFG_OK = 0,
    NETCFG_ERR_IP_FORMAT,
    NETCFG_ERR_MASK_FORMAT,
    NETCFG_ERR_GW_FORMAT,
    NETCFG_ERR_MASK_INVALID, // не суцільна або поза /8../30
    NETCFG_ERR_IP_RESERVED,  // 0.x, 127.x, 169.254.x, multicast/зарезервовані
    NETCFG_ERR_IP_HOST_PART, // адреса мережі або broadcast підмережі
    NETCFG_ERR_GW_SUBNET,    // шлюз поза підмережею
    NETCFG_ERR_GW_INVALID,   // шлюз збігається з IP, адресою мережі або broadcast
} netcfg_err_t;

void netcfg_default(netcfg_t *cfg);

// Строго "a.b.c.d": чотири десяткові октети 0..255, без пробілів, знаків і ведучих нулів.
bool netcfg_parse_ipv4(const char *s, uint32_t *out);

// buf має бути щонайменше 16 байтів.
void netcfg_format_ipv4(uint32_t addr, char *buf, size_t len);

netcfg_err_t netcfg_validate(const netcfg_t *cfg);

// Порожній gw означає без шлюзу. *out пишеться лише при NETCFG_OK.
netcfg_err_t netcfg_from_strings(const char *ip, const char *mask, const char *gw, netcfg_t *out);

// Стабільний машинний код, напр. для підсвічування хибного поля.
const char *netcfg_err_code(netcfg_err_t err);

// Повідомлення для користувача українською.
const char *netcfg_err_text(netcfg_err_t err);

#ifdef __cplusplus
}
#endif
