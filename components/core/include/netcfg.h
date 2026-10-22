#pragma once
// Network settings: parsing and validation of IP, mask and gateway.
// Addresses are host-order uint32_t: "a.b.c.d" == (a<<24)|(b<<16)|(c<<8)|d.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETCFG_DEFAULT_IP   0xC0A89024u // 192.168.144.36
#define NETCFG_DEFAULT_MASK 0xFFFFFF00u // 255.255.255.0
#define NETCFG_DEFAULT_GW   0x00000000u // no gateway

typedef struct {
    uint32_t ip;
    uint32_t mask;
    uint32_t gw; // 0 = no gateway
} netcfg_t;

typedef enum {
    NETCFG_OK = 0,
    NETCFG_ERR_IP_FORMAT,
    NETCFG_ERR_MASK_FORMAT,
    NETCFG_ERR_GW_FORMAT,
    NETCFG_ERR_MASK_INVALID, // not contiguous or outside /8../30
    NETCFG_ERR_IP_RESERVED,  // 0.x, 127.x, 169.254.x, multicast/reserved
    NETCFG_ERR_IP_HOST_PART, // network or broadcast address of the subnet
    NETCFG_ERR_GW_SUBNET,    // gateway outside the subnet
    NETCFG_ERR_GW_INVALID,   // gateway equals IP, network or broadcast address
} netcfg_err_t;

void netcfg_default(netcfg_t *cfg);

// Strict "a.b.c.d": four decimal octets 0..255, no spaces, signs or leading zeros.
bool netcfg_parse_ipv4(const char *s, uint32_t *out);

// buf needs at least 16 bytes.
void netcfg_format_ipv4(uint32_t addr, char *buf, size_t len);

netcfg_err_t netcfg_validate(const netcfg_t *cfg);

// Empty gw means no gateway. *out is written only on NETCFG_OK.
netcfg_err_t netcfg_from_strings(const char *ip, const char *mask, const char *gw, netcfg_t *out);

// Stable machine-readable code, e.g. for highlighting the offending field.
const char *netcfg_err_code(netcfg_err_t err);

// User-facing message in Ukrainian.
const char *netcfg_err_text(netcfg_err_t err);

#ifdef __cplusplus
}
#endif
