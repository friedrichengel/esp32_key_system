#include "net_eth.h"

#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_netif.h"
#include "mdns.h"

#include "config.h"
#include "settings.h"
#include "event_log.h"
#include "status_led.h"
#include "time_sync.h"

static const char *TAG = "net";

static esp_eth_handle_t s_eth;
static esp_netif_t     *s_netif;
static bool             s_link_up;
static bool             s_has_ip4;
static bool             s_mdns_started;
static uint8_t          s_mac[6];
static bool             s_fallback_active;
static esp_timer_handle_t s_dhcp_timer;

/* ------------------------------------------------------------------------
 * DHCP-Rückfall (wie check_dhcp_fallback() im Casambi-Gateway): Link da, DHCP aktiv,
 * nach CFG_DHCP_FALLBACK_TIMEOUT_S keine Adresse -> feste Adresse CFG_IPV4_FALLBACK_*.
 * Beim nächsten Link-up (Kabel neu gesteckt) wird DHCP erneut versucht.
 * ---------------------------------------------------------------------- */
static void apply_fallback_ip(void)
{
    esp_netif_ip_info_t ip = {0};
    esp_netif_str_to_ip4(CFG_IPV4_FALLBACK_ADDR, &ip.ip);
    esp_netif_str_to_ip4(CFG_IPV4_FALLBACK_MASK, &ip.netmask);
    esp_netif_str_to_ip4(CFG_IPV4_FALLBACK_GW, &ip.gw);
    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcpc_stop (Fallback): %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_ip_info(s_netif, &ip));
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_str_to_ip4(CFG_IPV4_FALLBACK_DNS, &dns.ip.u_addr.ip4);
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    s_fallback_active = true;
    event_log(EV_NETWORK, "Kein DHCP-Lease nach %d s, Rueckfall auf %s/%s", CFG_DHCP_FALLBACK_TIMEOUT_S,
              CFG_IPV4_FALLBACK_ADDR, CFG_IPV4_FALLBACK_MASK);
}

static void dhcp_timer_cb(void *arg)
{
    (void)arg;
    if (s_link_up && !s_has_ip4 && g_settings.net.ipv4_mode == IPV4_MODE_DHCP) {
        apply_fallback_ip();
    }
}

static void dhcp_watch_start(void)
{
    if (g_settings.net.ipv4_mode != IPV4_MODE_DHCP || !s_dhcp_timer) {
        return;
    }
    if (s_fallback_active) {
        /* Neuer Link: DHCP noch einmal versuchen */
        s_fallback_active = false;
        esp_err_t err = esp_netif_dhcpc_start(s_netif);
        ESP_LOGI(TAG, "DHCP erneut gestartet: %s", esp_err_to_name(err));
    }
    esp_timer_stop(s_dhcp_timer);
    esp_timer_start_once(s_dhcp_timer, (uint64_t)CFG_DHCP_FALLBACK_TIMEOUT_S * 1000000);
}

static void dhcp_watch_stop(void)
{
    if (s_dhcp_timer) {
        esp_timer_stop(s_dhcp_timer);
    }
}

/* ------------------------------------------------------------------------
 * YT8531: Autonegotiation nach dem Reset wieder einschalten und RGMII-Clock-Delays
 * (~2 ns RX/TX) über die Extended-Register setzen. Übernommen aus dem ESP-IDF-Beispiel
 * examples/ethernet/basic (Apache-2.0, Espressif).
 * ---------------------------------------------------------------------- */
static esp_err_t yt8531_specific_init(esp_eth_handle_t eth)
{
    bool auto_nego_en = true;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_S_AUTONEGO, &auto_nego_en), TAG, "autoneg");

    esp_eth_phy_reg_rw_data_t phy_reg = {.reg_value_p = NULL};
    uint32_t reg_val;
    phy_reg.reg_value_p = &reg_val;

    /* RX ~2 ns (EXT_CHIP_CONFIG 0xA001, Bit 8 rxc_dly_en) */
    reg_val = 0xA001;
    phy_reg.reg_addr = 0x1E;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_WRITE_PHY_REG, &phy_reg), TAG, "ext addr A001");
    phy_reg.reg_addr = 0x1F;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_READ_PHY_REG, &phy_reg), TAG, "read A001");
    reg_val |= (1U << 8);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_WRITE_PHY_REG, &phy_reg), TAG, "write A001");

    /* TX ~2 ns (EXT_RGMII_CONFIG1 0xA003, tx_delay_sel[3:0] und tx_delay_sel_fe[7:4] = 13) */
    reg_val = 0xA003;
    phy_reg.reg_addr = 0x1E;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_WRITE_PHY_REG, &phy_reg), TAG, "ext addr A003");
    phy_reg.reg_addr = 0x1F;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_READ_PHY_REG, &phy_reg), TAG, "read A003");
    reg_val = (reg_val & ~0x00FFU) | (13U << 4) | (13U << 0);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_WRITE_PHY_REG, &phy_reg), TAG, "write A003");

    ESP_LOGI(TAG, "YT8531: Autoneg an, RGMII-Delays RX/TX ~2 ns gesetzt");
    return ESP_OK;
}

static esp_err_t eth_driver_init(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CFG_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CFG_ETH_PHY_RST_GPIO;

    /* S31-Default ist bereits RGMII mit dem festen IO-MUX-Pinsatz (GPIO8-19, Clocks 13/14). */
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CFG_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CFG_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "MAC anlegen fehlgeschlagen");
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (!phy) {
        mac->del(mac);
        ESP_RETURN_ON_FALSE(false, ESP_FAIL, TAG, "PHY anlegen fehlgeschlagen");
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&config, &s_eth);
    if (err != ESP_OK) {
        mac->del(mac);
        phy->del(phy);
        ESP_LOGE(TAG, "Ethernet-Treiber: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(yt8531_specific_init(s_eth), TAG, "YT8531 init");
    esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, s_mac);
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * IPv4/IPv6 aus den Settings auf das netif anwenden (vor esp_eth_start).
 * ---------------------------------------------------------------------- */
static void apply_ipv4_settings(void)
{
    const settings_network_t *n = &g_settings.net;
    if (n->ipv4_mode != IPV4_MODE_STATIC) {
        return;
    }
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_str_to_ip4(n->ipv4_addr, &ip.ip) != ESP_OK ||
        esp_netif_str_to_ip4(n->ipv4_mask, &ip.netmask) != ESP_OK) {
        ESP_LOGE(TAG, "Statische IPv4 ungültig (%s / %s), bleibe bei DHCP", n->ipv4_addr, n->ipv4_mask);
        event_log(EV_NETWORK, "Statische IPv4 ungueltig, DHCP aktiv");
        return;
    }
    esp_netif_str_to_ip4(n->ipv4_gw, &ip.gw);   /* leer erlaubt */

    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_ip_info(s_netif, &ip));

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    if (n->dns1[0] && esp_netif_str_to_ip4(n->dns1, &dns.ip.u_addr.ip4) == ESP_OK) {
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    if (n->dns2[0] && esp_netif_str_to_ip4(n->dns2, &dns.ip.u_addr.ip4) == ESP_OK) {
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &dns);
    }
    ESP_LOGI(TAG, "Statische IPv4 %s/%s gw %s", n->ipv4_addr, n->ipv4_mask, n->ipv4_gw);
}

static void apply_ipv6_static(void)
{
    const settings_network_t *n = &g_settings.net;
    if (!n->ipv6_static_enable || !n->ipv6_addr[0]) {
        return;
    }
    esp_ip6_addr_t a = {0};
    if (esp_netif_str_to_ip6(n->ipv6_addr, &a) != ESP_OK) {
        ESP_LOGE(TAG, "Statische IPv6 ungültig: %s", n->ipv6_addr);
        event_log(EV_NETWORK, "Statische IPv6 ungueltig");
        return;
    }
    esp_err_t err = esp_netif_add_ip6_address(s_netif, a, true);
    ESP_LOGI(TAG, "Statische IPv6 %s/%u: %s", n->ipv6_addr, n->ipv6_prefix_len, esp_err_to_name(err));
}

/* ------------------------------------------------------------------------
 * mDNS
 * ---------------------------------------------------------------------- */
static void mdns_start(void)
{
    if (s_mdns_started) {
        return;
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(g_settings.net.hostname);
    mdns_instance_name_set("ESP32 Key System");
    mdns_service_add(NULL, "_https", "_tcp", CFG_HTTPS_PORT, NULL, 0);
    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS: %s.local", g_settings.net.hostname);
}

/* ------------------------------------------------------------------------
 * Events
 * ---------------------------------------------------------------------- */
static void update_led(void)
{
    if (!s_link_up) {
        status_led_set(LED_NO_LINK);
    } else if (!s_has_ip4) {
        status_led_set(LED_NO_IP);
    } else {
        status_led_set(LED_OK);
    }
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        s_link_up = true;
        net_status_t st;
        net_eth_get_status(&st);
        event_log(EV_NETWORK, "Ethernet Link up, %u Mbit/s %s", st.speed_mbps,
                  st.full_duplex ? "Vollduplex" : "Halbduplex");
        esp_netif_create_ip6_linklocal(s_netif);
        apply_ipv6_static();
        dhcp_watch_start();
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        s_has_ip4 = false;
        dhcp_watch_stop();
        event_log(EV_NETWORK, "Ethernet Link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet gestartet");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet gestoppt");
        break;
    default:
        break;
    }
    update_led();
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case IP_EVENT_ETH_GOT_IP: {
        const ip_event_got_ip_t *ev = data;
        s_has_ip4 = true;
        dhcp_watch_stop();
        event_log(EV_NETWORK, "IPv4 " IPSTR " / " IPSTR " gw " IPSTR "%s",
                  IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask), IP2STR(&ev->ip_info.gw),
                  s_fallback_active ? " (Rueckfalladresse)" : "");
        mdns_start();
        time_sync_start();
        break;
    }
    case IP_EVENT_ETH_LOST_IP:
        s_has_ip4 = false;
        event_log(EV_NETWORK, "IPv4 verloren");
        break;
    case IP_EVENT_GOT_IP6: {
        const ip_event_got_ip6_t *ev = data;
        esp_ip6_addr_type_t type = esp_netif_ip6_get_addr_type((esp_ip6_addr_t *)&ev->ip6_info.ip);
        event_log(EV_NETWORK, "IPv6 " IPV6STR " (%s)", IPV62STR(ev->ip6_info.ip),
                  type == ESP_IP6_ADDR_IS_LINK_LOCAL ? "link-local" :
                  type == ESP_IP6_ADDR_IS_GLOBAL ? "global" :
                  type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL ? "ULA" : "sonstige");
        if (type != ESP_IP6_ADDR_IS_LINK_LOCAL) {
            mdns_start();
            time_sync_start();
        }
        break;
    }
    default:
        break;
    }
    update_led();
}

/* ------------------------------------------------------------------------
 * Öffentliche API
 * ---------------------------------------------------------------------- */
esp_err_t net_eth_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_ERROR(eth_driver_init(), TAG, "eth driver");

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif anlegen");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)), TAG, "attach");

    esp_netif_set_hostname(s_netif, g_settings.net.hostname);
    apply_ipv4_settings();

    const esp_timer_create_args_t targs = { .callback = dhcp_timer_cb, .name = "dhcp_fallback" };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_create(&targs, &s_dhcp_timer));

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL), TAG, "eth evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL), TAG, "ip evt");

    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth), TAG, "eth start");
    ESP_LOGI(TAG, "Ethernet gestartet, MAC %02x:%02x:%02x:%02x:%02x:%02x, Hostname %s",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5], g_settings.net.hostname);
    update_led();
    return ESP_OK;
}

void net_eth_get_status(net_status_t *st)
{
    memset(st, 0, sizeof(*st));
    memcpy(st->mac, s_mac, 6);
    strlcpy(st->hostname, g_settings.net.hostname, sizeof(st->hostname));
    st->link_up = s_link_up;
    st->has_ip4 = s_has_ip4;
    st->ip4_fallback = s_fallback_active && s_has_ip4;

    if (s_eth && s_link_up) {
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        if (esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed) == ESP_OK) {
            st->speed_mbps = (speed == ETH_SPEED_1000M) ? 1000 : (speed == ETH_SPEED_100M) ? 100 : 10;
        }
        if (esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &duplex) == ESP_OK) {
            st->full_duplex = (duplex == ETH_DUPLEX_FULL);
        }
    }
    if (!s_netif) {
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(st->ip4, sizeof(st->ip4), IPSTR, IP2STR(&ip.ip));
        snprintf(st->mask4, sizeof(st->mask4), IPSTR, IP2STR(&ip.netmask));
        snprintf(st->gw4, sizeof(st->gw4), IPSTR, IP2STR(&ip.gw));
    }
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4) {
        snprintf(st->dns1, sizeof(st->dns1), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4) {
        snprintf(st->dns2, sizeof(st->dns2), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    esp_ip6_addr_t ip6[NET_MAX_IP6];
    int n = esp_netif_get_all_ip6(s_netif, ip6);
    if (n > NET_MAX_IP6) {
        n = NET_MAX_IP6;
    }
    st->ip6_count = n;
    for (int i = 0; i < n; i++) {
        snprintf(st->ip6[i], sizeof(st->ip6[i]), IPV6STR, IPV62STR(ip6[i]));
    }
}

bool net_eth_has_ip(void)
{
    return s_has_ip4;
}

esp_netif_t *net_eth_get_netif(void)
{
    return s_netif;
}
