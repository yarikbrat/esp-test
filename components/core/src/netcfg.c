#include "netcfg.h"

#include <stdio.h>

void netcfg_default(netcfg_t *cfg)
{
    cfg->ip = NETCFG_DEFAULT_IP;
    cfg->mask = NETCFG_DEFAULT_MASK;
    cfg->gw = NETCFG_DEFAULT_GW;
}

bool netcfg_parse_ipv4(const char *s, uint32_t *out)
{
    if (!s) {
        return false;
    }
    uint32_t addr = 0;
    for (int octet = 0; octet < 4; octet++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        unsigned v = 0;
        int digits = 0;
        const char *start = s;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3) {
                return false;
            }
            v = v * 10 + (unsigned)(*s - '0');
            s++;
        }
        if (v > 255 || (digits > 1 && *start == '0')) {
            return false;
        }
        addr = (addr << 8) | v;
        if (octet < 3) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    if (*s != '\0') {
        return false;
    }
    *out = addr;
    return true;
}

void netcfg_format_ipv4(uint32_t a, char *buf, size_t len)
{
    snprintf(buf, len, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 0xFF),
             (unsigned)((a >> 8) & 0xFF), (unsigned)(a & 0xFF));
}

static bool mask_contiguous(uint32_t mask)
{
    uint32_t inv = ~mask;
    return (inv & (inv + 1)) == 0; // inverted contiguous mask + 1 is a power of two
}

static int mask_prefix(uint32_t mask)
{
    int n = 0;
    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}

static bool addr_reserved(uint32_t a)
{
    uint8_t first = (uint8_t)(a >> 24);
    return first == 0 || first == 127 || first >= 224 || (a >> 16) == 0xA9FE;
}

netcfg_err_t netcfg_validate(const netcfg_t *cfg)
{
    if (!mask_contiguous(cfg->mask)) {
        return NETCFG_ERR_MASK_INVALID;
    }
    int prefix = mask_prefix(cfg->mask);
    if (prefix < 8 || prefix > 30) {
        return NETCFG_ERR_MASK_INVALID;
    }
    if (addr_reserved(cfg->ip)) {
        return NETCFG_ERR_IP_RESERVED;
    }
    uint32_t host = cfg->ip & ~cfg->mask;
    if (host == 0 || host == ~cfg->mask) {
        return NETCFG_ERR_IP_HOST_PART;
    }
    if (cfg->gw != 0) {
        if ((cfg->gw & cfg->mask) != (cfg->ip & cfg->mask)) {
            return NETCFG_ERR_GW_SUBNET;
        }
        uint32_t gw_host = cfg->gw & ~cfg->mask;
        if (cfg->gw == cfg->ip || gw_host == 0 || gw_host == ~cfg->mask) {
            return NETCFG_ERR_GW_INVALID;
        }
    }
    return NETCFG_OK;
}

netcfg_err_t netcfg_from_strings(const char *ip, const char *mask, const char *gw, netcfg_t *out)
{
    netcfg_t c;
    if (!netcfg_parse_ipv4(ip, &c.ip)) {
        return NETCFG_ERR_IP_FORMAT;
    }
    if (!netcfg_parse_ipv4(mask, &c.mask)) {
        return NETCFG_ERR_MASK_FORMAT;
    }
    if (!gw || gw[0] == '\0') {
        c.gw = 0;
    } else if (!netcfg_parse_ipv4(gw, &c.gw)) {
        return NETCFG_ERR_GW_FORMAT;
    }
    netcfg_err_t err = netcfg_validate(&c);
    if (err == NETCFG_OK) {
        *out = c;
    }
    return err;
}

const char *netcfg_err_code(netcfg_err_t err)
{
    switch (err) {
    case NETCFG_OK: return "ok";
    case NETCFG_ERR_IP_FORMAT: return "ip_format";
    case NETCFG_ERR_MASK_FORMAT: return "mask_format";
    case NETCFG_ERR_GW_FORMAT: return "gw_format";
    case NETCFG_ERR_MASK_INVALID: return "mask_invalid";
    case NETCFG_ERR_IP_RESERVED: return "ip_reserved";
    case NETCFG_ERR_IP_HOST_PART: return "ip_host_part";
    case NETCFG_ERR_GW_SUBNET: return "gw_subnet";
    case NETCFG_ERR_GW_INVALID: return "gw_invalid";
    }
    return "unknown";
}

const char *netcfg_err_text(netcfg_err_t err)
{
    switch (err) {
    case NETCFG_OK: return "OK";
    case NETCFG_ERR_IP_FORMAT: return "Невірний формат IP-адреси";
    case NETCFG_ERR_MASK_FORMAT: return "Невірний формат маски";
    case NETCFG_ERR_GW_FORMAT: return "Невірний формат шлюзу";
    case NETCFG_ERR_MASK_INVALID: return "Маска має бути суцільною, від /8 до /30";
    case NETCFG_ERR_IP_RESERVED: return "Зарезервована адреса (0.x, 127.x, 169.254.x, multicast)";
    case NETCFG_ERR_IP_HOST_PART: return "IP збігається з адресою мережі або broadcast";
    case NETCFG_ERR_GW_SUBNET: return "Шлюз не належить підмережі";
    case NETCFG_ERR_GW_INVALID: return "Шлюз збігається з IP, адресою мережі або broadcast";
    }
    return "Невідома помилка";
}
