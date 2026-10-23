/*
 * net_eth.h – Ethernet (ESP32-S31 EMAC, RGMII, YT8531-PHY), IPv4/IPv6, Hostname, mDNS
 *
 * Konfiguration kommt aus g_settings.net. Änderungen an IP-Einstellungen werden
 * durch Neustart übernommen (net_eth_apply_* in Phase 3 über die Web-UI).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#define NET_MAX_IP6 CONFIG_LWIP_IPV6_NUM_ADDRESSES

typedef struct {
    bool     link_up;
    uint16_t speed_mbps;           /* 10 / 100 / 1000, 0 = unbekannt */
    bool     full_duplex;
    uint8_t  mac[6];
    bool     has_ip4;
    bool     ip4_fallback;         /* DHCP lieferte nichts, feste Rückfalladresse aktiv */
    char     ip4[16];
    char     mask4[16];
    char     gw4[16];
    char     dns1[16];
    char     dns2[16];
    int      ip6_count;
    char     ip6[NET_MAX_IP6][48];
    char     hostname[64];
} net_status_t;

esp_err_t net_eth_start(void);
void      net_eth_get_status(net_status_t *st);
bool      net_eth_has_ip(void);
esp_netif_t *net_eth_get_netif(void);
