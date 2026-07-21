#include "mp_wifi.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "mp_config.h"

#define WIFI_CONNECTED BIT0

static StaticEventGroup_t s_event_buffer;
static EventGroupHandle_t s_event;

static void wifi_event(void *argument, esp_event_base_t base, int32_t id, void *data);
esp_err_t mp_wifi_start(void);
bool mp_wifi_wait(TickType_t timeout);
bool mp_wifi_connected(void);

static void wifi_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_event, WIFI_CONNECTED);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_event, WIFI_CONNECTED);
    }
}

esp_err_t mp_wifi_start(void)
{
    const mp_config_t *config = mp_config_get();
    if (!config->wifi_ssid[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    setenv("TZ", config->timezone, 1);
    tzset();
    s_event = xEventGroupCreateStatic(&s_event_buffer);
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK) {
        return error;
    }
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&init);
    if (error != ESP_OK) {
        return error;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    wifi_config_t wifi = {0};
    strlcpy((char *)wifi.sta.ssid, config->wifi_ssid, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, config->wifi_password, sizeof(wifi.sta.password));
    wifi.sta.threshold.authmode = config->wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi.sta.pmf_cfg.capable = true;
    wifi.sta.pmf_cfg.required = false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if ((error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (error = esp_wifi_set_config(WIFI_IF_STA, &wifi)) != ESP_OK ||
        (error = esp_wifi_start()) != ESP_OK) {
        return error;
    }
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    return esp_netif_sntp_init(&sntp);
}

bool mp_wifi_wait(TickType_t timeout)
{
    return s_event && (xEventGroupWaitBits(s_event, WIFI_CONNECTED, pdFALSE, pdTRUE, timeout) & WIFI_CONNECTED);
}

bool mp_wifi_connected(void)
{
    return s_event && (xEventGroupGetBits(s_event) & WIFI_CONNECTED);
}
