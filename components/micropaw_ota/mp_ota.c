#include "mp_ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "sdkconfig.h"

static const char s_url[] =
    "https://github.com/mahiatlinux/MicroPaw/releases/latest/download/micropaw.bin";

static void report(mp_ota_progress_fn progress, void *context, const char *text);
static bool valid_description(const esp_app_desc_t *description);
static bool pending_verify(void);
esp_err_t mp_ota_update(mp_ota_progress_fn progress, void *context,
                        char *output, size_t size);
esp_err_t mp_ota_confirm_running(void);
esp_err_t mp_ota_rollback_pending(void);

static void report(mp_ota_progress_fn progress, void *context, const char *text)
{
    if (progress) {
        progress(text, context);
    }
}

static bool valid_description(const esp_app_desc_t *description)
{
    return description->magic_word == ESP_APP_DESC_MAGIC_WORD &&
           strcmp(description->project_name, "micropaw") == 0 &&
           description->version[0] && description->idf_ver[0] &&
           description->date[0] && description->time[0];
}

static bool pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t mp_ota_update(mp_ota_progress_fn progress, void *context,
                        char *output, size_t size)
{
    if (!output || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    report(progress, context, "OTA: connecting to the latest signed release.");
    esp_http_client_config_t http = {
        .url = s_url,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .keep_alive_idle = 20,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        .save_client_session = true,
#endif
    };
    esp_https_ota_config_t config = {
        .http_config = &http
    };
    esp_https_ota_handle_t handle = NULL;
    esp_err_t error = esp_https_ota_begin(&config, &handle);
    if (error != ESP_OK) {
        snprintf(output, size, "OTA connection failed: %s.", esp_err_to_name(error));
        return error;
    }
    esp_app_desc_t incoming;
    error = esp_https_ota_get_img_desc(handle, &incoming);
    if (error != ESP_OK || !valid_description(&incoming)) {
        esp_https_ota_abort(handle);
        strlcpy(output, "OTA image description or project name is invalid.", size);
        return error == ESP_OK ? ESP_ERR_OTA_VALIDATE_FAILED : error;
    }
    esp_app_desc_t running;
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    error = running_partition ?
            esp_ota_get_partition_description(running_partition, &running) :
            ESP_ERR_NOT_FOUND;
    if (error != ESP_OK) {
        esp_https_ota_abort(handle);
        snprintf(output, size, "Could not read the running image description: %s.",
                 esp_err_to_name(error));
        return error;
    }
    if (strcmp(incoming.version, running.version) == 0) {
        esp_https_ota_abort(handle);
        snprintf(output, size, "Already running version %s. Update skipped.", running.version);
        return ESP_ERR_NOT_FOUND;
    }
    int image_size = esp_https_ota_get_image_size(handle);
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target || image_size <= 0 || (size_t)image_size > target->size) {
        esp_https_ota_abort(handle);
        strlcpy(output, "OTA image does not fit the inactive 2 MiB slot.", size);
        return ESP_ERR_INVALID_SIZE;
    }
    char status[192];
    snprintf(status, sizeof(status),
             "OTA: description validated for project=micropaw version=%s idf=%s size=%d bytes.",
             incoming.version, incoming.idf_ver, image_size);
    report(progress, context, status);
    int last_percent = 0;
    bool chip_reported = false;
    while ((error = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (!chip_reported) {
            report(progress, context, "OTA: ESP32-S3 chip validation passed.");
            chip_reported = true;
        }
        int received = esp_https_ota_get_image_len_read(handle);
        int percent = received > 0 ? received * 100 / image_size : 0;
        if (percent >= last_percent + 10 || percent == 100) {
            snprintf(status, sizeof(status), "OTA: downloaded %d/%d bytes (%d%%).",
                     received, image_size, percent);
            report(progress, context, status);
            last_percent = percent;
        }
    }
    if (error != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        snprintf(output, size, "OTA download failed: %s.", esp_err_to_name(error));
        return error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
    }
    error = esp_https_ota_finish(handle);
    if (error != ESP_OK) {
        snprintf(output, size, "OTA RSA-3072 signature verification failed: %s.",
                 esp_err_to_name(error));
        return error;
    }
    snprintf(output, size,
             "OTA complete. RSA-3072 signature verified for version %s. Rebooting into the inactive slot.",
             incoming.version);
    return ESP_OK;
}

esp_err_t mp_ota_confirm_running(void)
{
    return pending_verify() ? esp_ota_mark_app_valid_cancel_rollback() : ESP_OK;
}

esp_err_t mp_ota_rollback_pending(void)
{
    return pending_verify() ? esp_ota_mark_app_invalid_rollback_and_reboot() : ESP_OK;
}
