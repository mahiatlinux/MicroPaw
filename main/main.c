#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "esp_attr.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mp_agent.h"
#include "mp_config.h"
#include "mp_memory.h"
#include "mp_metrics.h"
#include "mp_net.h"
#include "mp_scheduler.h"
#include "mp_telegram.h"
#include "mp_tools.h"
#include "mp_wifi.h"
#include "sdkconfig.h"

static StaticTask_t s_cli_task_buffer;
static StackType_t s_cli_stack[CONFIG_MICROPAW_CLI_STACK];
static TaskHandle_t s_cli_task;
EXT_RAM_BSS_ATTR static char s_config_toml[MP_CONFIG_TOML_MAX + 1];
static const char *TAG = "micropaw";

static esp_err_t send_output(const char *chat_id, const char *text);
static esp_err_t schedule_output(const char *chat_id, const char *text);
static esp_err_t setup_console(void);
static void cli_task(void *argument);
static void cli_line(char *line);
static void receive_config(size_t length);
static esp_err_t initialize(void);
void app_main(void);

static esp_err_t send_output(const char *chat_id, const char *text)
{
#if CONFIG_MICROPAW_TELEGRAM
    const mp_config_t *config = mp_config_get();
    if (config->telegram_token[0] && strcmp(chat_id, "serial") != 0) {
        return mp_telegram_send(chat_id, text);
    }
#endif
    ESP_LOGI("reply", "%s", text);
    return ESP_OK;
}

static esp_err_t schedule_output(const char *chat_id, const char *text)
{
    return mp_agent_submit(chat_id, text, true, pdMS_TO_TICKS(100));
}

static esp_err_t setup_console(void)
{
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    esp_err_t error = usb_serial_jtag_driver_install(&config);
    if (error != ESP_OK) {
        return error;
    }
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    fcntl(fileno(stdin), F_SETFL, 0);
    setvbuf(stdin, NULL, _IONBF, 0);
    return ESP_OK;
}

static void cli_task(void *argument)
{
    (void)argument;
    char line[640];
    while (fgets(line, sizeof(line), stdin)) {
        cli_line(line);
    }
    vTaskDelete(NULL);
}

static void receive_config(size_t length)
{
    printf("CONFIG READY\n");
    fflush(stdout);
    size_t received = 0;
    while (received < length) {
        size_t count = fread(s_config_toml + received, 1, length - received, stdin);
        if (!count) {
            clearerr(stdin);
            printf("CONFIG ERROR read\n");
            return;
        }
        received += count;
    }
    s_config_toml[length] = 0;
    size_t error_line = 0;
    esp_err_t error = mp_config_import_toml(s_config_toml, length, &error_line);
    memset(s_config_toml, 0, length);
    if (error == ESP_OK) {
        printf("CONFIG OK\n");
    } else if (error_line) {
        printf("CONFIG ERROR %s line=%u\n", esp_err_to_name(error), (unsigned)error_line);
    } else {
        printf("CONFIG ERROR %s\n", esp_err_to_name(error));
    }
    fflush(stdout);
}

static void cli_line(char *line)
{
    line[strcspn(line, "\r\n")] = 0;
    if (strcmp(line, "config") == 0) {
        char output[1024];
        mp_config_format(output, sizeof(output));
        printf("%s", output);
    } else if (strncmp(line, "set ", 4) == 0) {
        char *key = line + 4;
        char *value = strchr(key, ' ');
        if (!value) {
            printf("usage: set KEY VALUE\n");
            return;
        }
        *value++ = 0;
        esp_err_t error = mp_config_set(key, value);
        printf("%s\n", error == ESP_OK ? "saved; reboot to apply network changes" : esp_err_to_name(error));
    } else if (strncmp(line, "push-config ", 12) == 0) {
        char *end;
        unsigned long length = strtoul(line + 12, &end, 10);
        while (*end == ' ') {
            end++;
        }
        if (end == line + 12 || *end || length == 0 || length > MP_CONFIG_TOML_MAX) {
            printf("usage: push-config BYTES (1-%u)\n", MP_CONFIG_TOML_MAX);
        } else {
            receive_config((size_t)length);
        }
    } else if (strcmp(line, "erase-config YES") == 0) {
        printf("%s\n", esp_err_to_name(mp_config_erase()));
    } else if (strcmp(line, "metrics") == 0) {
        char output[768];
        mp_metrics_format(output, sizeof(output));
        printf("%s", output);
    } else if (strncmp(line, "submit ", 7) == 0) {
        printf("%s\n", esp_err_to_name(mp_agent_submit("serial", line + 7, false, pdMS_TO_TICKS(100))));
    } else if (strcmp(line, "reboot") == 0) {
        esp_restart();
    } else if (line[0]) {
        printf("commands: config, set KEY VALUE, push-config BYTES, erase-config YES, metrics, submit TEXT, reboot\n");
    }
}

static esp_err_t initialize(void)
{
    esp_err_t error;
    if ((error = setup_console()) != ESP_OK ||
        (error = mp_config_init()) != ESP_OK ||
        (error = mp_memory_init()) != ESP_OK ||
        (error = mp_net_init()) != ESP_OK ||
        (error = mp_tools_init()) != ESP_OK ||
        (error = mp_agent_init(send_output)) != ESP_OK ||
        (error = mp_scheduler_init(schedule_output)) != ESP_OK ||
        (error = mp_agent_start()) != ESP_OK ||
        (error = mp_scheduler_start()) != ESP_OK) {
        return error;
    }
    error = mp_wifi_start();
    if (error == ESP_OK) {
#if CONFIG_MICROPAW_TELEGRAM
        esp_err_t telegram_error = mp_telegram_start();
        if (telegram_error != ESP_OK) {
            ESP_LOGW(TAG, "Telegram disabled at runtime: %s", esp_err_to_name(telegram_error));
        }
#endif
    } else {
        ESP_LOGW(TAG, "Wi-Fi disabled at runtime: %s", esp_err_to_name(error));
    }
    s_cli_task = xTaskCreateStaticPinnedToCore(cli_task, "mp_cli", CONFIG_MICROPAW_CLI_STACK,
                                               NULL, 3, s_cli_stack, &s_cli_task_buffer, 0);
    if (!s_cli_task) {
        return ESP_ERR_NO_MEM;
    }
    mp_metrics_register("cli", s_cli_task);
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t error = initialize();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "init: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG, "MicroPaw ready. Use the serial config command before adding secrets.");
    mp_metrics_log();
}
