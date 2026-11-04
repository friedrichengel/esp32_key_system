/*
 * net_eth.h – Netzwerk: Ethernet (ESP32-S31 EMAC, RGMII, YT8531-PHY) immer, WLAN-Station
 * optional zusätzlich; IPv4/IPv6, Hostname, mDNS
 *
 * Konfiguration kommt aus g_settings.net (IP, Hostname, NTP; gilt für Ethernet) und
 * g_settings.wifi (ab Werk aus). Ethernet läuft immer. Ist WLAN eingeschaltet und eine SSID
 * gesetzt, kommt die WLAN-Station als zweite Schnittstelle dazu (nur DHCP/SLAAC, kein SoftAP).
 * Änderungen werden durch Neustart übernommen.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#define NET_MAX_IP6 CONFIG_LWIP_IPV6_NUM_ADDRESSES

typedef struct {
    /* Ethernet */
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

    /* WLAN-Station (nur wenn aktiviert) */
    bool     wifi_enabled;         /* Treiber läuft                               */
    bool     wifi_connected;       /* mit dem Access Point verbunden              */
    bool     wifi_has_ip4;
    char     wifi_ssid[33];
    int8_t   wifi_rssi;            /* dBm, 0 = unbekannt                           */
    uint8_t  wifi_channel;
    uint8_t  wifi_mac[6];
    char     wifi_ip4[16];
    int      wifi_ip6_count;
    char     wifi_ip6[NET_MAX_IP6][48];
} net_status_t;

typedef struct {
    char     ssid[33];
    int8_t   rssi;
    uint8_t  channel;
    bool     open;                 /* ohne Verschlüsselung */
} net_wifi_ap_t;

esp_err_t net_eth_start(void);
void      net_eth_get_status(net_status_t *st);
bool      net_eth_has_ip(void);              /* irgendeine Schnittstelle hat IPv4 */
bool      net_eth_dns_static(void);          /* feste DNS-Server eingetragen (gilt auch bei DHCP) */
esp_netif_t *net_eth_get_netif(void);        /* Ethernet-netif */
/* WLAN-Umgebung scannen (blockierend, einige Sekunden). Läuft WLAN nicht, wird der Treiber
 * nur für den Scan gestartet und danach wieder beendet. Doppelte SSIDs werden zusammengefasst,
 * Ergebnis nach Pegel sortiert. ESP_ERR_INVALID_STATE wenn gerade ein Scan läuft. */
esp_err_t net_wifi_scan(net_wifi_ap_t *out, int max, int *count);
