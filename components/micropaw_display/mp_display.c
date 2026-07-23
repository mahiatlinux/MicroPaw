#include "mp_display.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp_config.h"
#include "mp_metrics.h"
#include "mp_scheduler.h"
#include "sdkconfig.h"

#define OLED_WIDTH 128
#define OLED_MAX_HEIGHT 64
#define OLED_ADDRESS 0x3c
#define OLED_SDA GPIO_NUM_8
#define OLED_SCL GPIO_NUM_9
#define OLED_BUTTON GPIO_NUM_0

typedef enum {
    BUTTON_NONE,
    BUTTON_CLICK,
    BUTTON_HOLD
} button_event_t;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static esp_timer_handle_t s_wake_timer;
static TaskHandle_t s_task;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile mp_display_activity_t s_activity = MP_DISPLAY_IDLE;
static volatile mp_display_mood_t s_mood = MP_DISPLAY_HAPPY;
static bool s_agent_busy;
static bool s_response_pending;
static volatile bool s_enabled;
static uint8_t s_height = OLED_MAX_HEIGHT;
static bool s_wake_ready;
static int64_t s_wake_pressed_us;
static const char *TAG = "display";

static void display_task(void *argument);
static esp_err_t display_task_start(void);
static void wake_timer_callback(void *argument);
static esp_err_t display_init(void);
static void display_deinit(void);
static esp_err_t oled_commands(const uint8_t *commands, size_t count);
static esp_err_t oled_clear(void);
static esp_err_t oled_write(const uint8_t *frame);
static void pixel(uint8_t *frame, int x, int y);
static void line(uint8_t *frame, int x0, int y0, int x1, int y1);
static void circle(uint8_t *frame, int center_x, int center_y, int radius);
static void fill_circle(uint8_t *frame, int center_x, int center_y, int radius);
static void fill_rect(uint8_t *frame, int x, int y, int width, int height);
static void draw_text(uint8_t *frame, int x, int y, const char *text);
static size_t draw_wrapped(uint8_t *frame, int y, const char *text, int lines);
static void draw_eyes(uint8_t *frame, bool blink, int offset_x, int offset_y);
static void draw_smile(uint8_t *frame);
static void draw_speaking_mouth(uint8_t *frame, uint32_t animation);
static void draw_face(uint8_t *frame, mp_display_activity_t activity,
                      mp_display_mood_t mood, uint32_t animation, bool blink);
static size_t draw_jobs(uint8_t *frame, size_t index, size_t count, size_t text_offset);
static button_event_t button_event(int64_t now, int *raw_level, int *stable_level,
                                   int64_t *changed_us, int64_t *pressed_us,
                                   bool *held, bool allow_hold);
static void display_notify(void);
esp_err_t mp_display_start(void);
void mp_display_agent_begin(void);
void mp_display_agent_end(void);
void mp_display_response_begin(void);
void mp_display_response_end(void);
void mp_display_filter_text(char *text);
bool mp_display_enabled(void);

static esp_err_t display_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = OLED_SDA,
        .scl_io_num = OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };
    esp_err_t error = i2c_new_master_bus(&bus_config, &s_bus);
    if (error != ESP_OK) {
        return error;
    }
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDRESS,
        .scl_speed_hz = 400000
    };
    error = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (error != ESP_OK) {
        display_deinit();
        return error;
    }
    uint8_t init[] = {
        0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40, 0x8d, 0x14,
        0x20, 0x02, 0xa1, 0xc8, 0xda, 0x12, 0x81, 0xcf, 0xd9, 0xf1,
        0xdb, 0x40, 0xa4, 0xa6
    };
    init[4] = s_height - 1;
    init[15] = s_height == 32 ? 0x02 : 0x12;
    error = oled_commands(init, sizeof(init));
    if (error != ESP_OK) {
        display_deinit();
        return error;
    }
    error = oled_clear();
    if (error == ESP_OK) {
        uint8_t on = 0xaf;
        error = oled_commands(&on, 1);
    }
    if (error != ESP_OK) {
        display_deinit();
    }
    return error;
}

static void display_deinit(void)
{
    if (s_device) {
        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

static esp_err_t oled_commands(const uint8_t *commands, size_t count)
{
    uint8_t packet[32];
    if (!commands || count + 1 > sizeof(packet)) {
        return ESP_ERR_INVALID_ARG;
    }
    packet[0] = 0;
    memcpy(packet + 1, commands, count);
    return i2c_master_transmit(s_device, packet, count + 1, 100);
}

static esp_err_t oled_clear(void)
{
    uint8_t data[OLED_WIDTH + 1] = {0};
    data[0] = 0x40;
    for (uint8_t page = 0; page < s_height / 8; page++) {
        uint8_t position[] = {(uint8_t)(0xb0 | page), 0x00, 0x10};
        esp_err_t error = oled_commands(position, sizeof(position));
        if (error != ESP_OK) {
            return error;
        }
        error = i2c_master_transmit(s_device, data, sizeof(data), 100);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

static esp_err_t oled_write(const uint8_t *frame)
{
    uint8_t data[OLED_WIDTH + 1];
    data[0] = 0x40;
    for (uint8_t page = 0; page < s_height / 8; page++) {
        uint8_t position[] = {(uint8_t)(0xb0 | page), 0x00, 0x10};
        esp_err_t error = oled_commands(position, sizeof(position));
        if (error != ESP_OK) {
            return error;
        }
        memcpy(data + 1, frame + page * OLED_WIDTH, OLED_WIDTH);
        error = i2c_master_transmit(s_device, data, sizeof(data), 100);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

static void pixel(uint8_t *frame, int x, int y)
{
    if (x >= 0 && x < OLED_WIDTH && y >= 0 && y < s_height) {
        frame[(y / 8) * OLED_WIDTH + x] |= 1U << (y & 7);
    }
}

static void line(uint8_t *frame, int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        pixel(frame, x0, y0);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void circle(uint8_t *frame, int center_x, int center_y, int radius)
{
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        pixel(frame, center_x + x, center_y + y);
        pixel(frame, center_x + y, center_y + x);
        pixel(frame, center_x - y, center_y + x);
        pixel(frame, center_x - x, center_y + y);
        pixel(frame, center_x - x, center_y - y);
        pixel(frame, center_x - y, center_y - x);
        pixel(frame, center_x + y, center_y - x);
        pixel(frame, center_x + x, center_y - y);
        y++;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            x--;
            error += 2 * (y - x) + 1;
        }
    }
}

static void fill_circle(uint8_t *frame, int center_x, int center_y, int radius)
{
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) {
                pixel(frame, center_x + x, center_y + y);
            }
        }
    }
}

static void fill_rect(uint8_t *frame, int x, int y, int width, int height)
{
    for (int row = y; row < y + height; row++) {
        for (int column = x; column < x + width; column++) {
            pixel(frame, column, row);
        }
    }
}

static void draw_text(uint8_t *frame, int x, int y, const char *text)
{
    while (*text && x + 5 <= OLED_WIDTH) {
        unsigned char character = (unsigned char)toupper((unsigned char)*text++);
        if (character < ' ' || character > 'Z') {
            character = '?';
        }
        const uint8_t *rows = s_font[character - ' '];
        for (int row = 0; row < 7; row++) {
            for (int column = 0; column < 5; column++) {
                if (rows[row] & (1U << (4 - column))) {
                    pixel(frame, x + column, y + row);
                }
            }
        }
        x += 6;
    }
}

static size_t draw_wrapped(uint8_t *frame, int y, const char *text, int lines)
{
    const char *start = text;
    char row[22];
    for (int line_index = 0; line_index < lines && *text; line_index++) {
        while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') {
            text++;
        }
        if (!*text) {
            break;
        }
        size_t length = 0;
        size_t space = 0;
        while (text[length] && text[length] != '\n' && text[length] != '\r' &&
               length < sizeof(row) - 1) {
            if (text[length] == ' ') {
                space = length;
            }
            length++;
        }
        if (length == sizeof(row) - 1 && text[length] && space) {
            length = space;
        }
        memcpy(row, text, length);
        row[length] = 0;
        draw_text(frame, 1, y + line_index * 8, row);
        text += length;
    }
    while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') {
        text++;
    }
    return *text ? (size_t)(text - start) : 0;
}

static void draw_eyes(uint8_t *frame, bool blink, int offset_x, int offset_y)
{
    bool short_panel = s_height == 32;
    if (blink) {
        fill_rect(frame, short_panel ? 28 : 25, short_panel ? 8 : 23,
                  short_panel ? 18 : 24, short_panel ? 3 : 4);
        fill_rect(frame, short_panel ? 82 : 79, short_panel ? 8 : 23,
                  short_panel ? 18 : 24, short_panel ? 3 : 4);
        return;
    }
    fill_circle(frame, 37 + offset_x, (short_panel ? 9 : 24) + offset_y,
                short_panel ? 4 : 6);
    fill_circle(frame, 91 + offset_x, (short_panel ? 9 : 24) + offset_y,
                short_panel ? 4 : 6);
}

static void draw_smile(uint8_t *frame)
{
    bool short_panel = s_height == 32;
    int top = short_panel ? 21 : 46;
    int middle = short_panel ? 24 : 50;
    int bottom = short_panel ? 25 : 52;
    line(frame, 43, top, 50, middle);
    line(frame, 50, middle, 57, bottom);
    line(frame, 57, bottom, 71, bottom);
    line(frame, 71, bottom, 78, middle);
    line(frame, 78, middle, 85, top);
}

static void draw_speaking_mouth(uint8_t *frame, uint32_t animation)
{
    bool short_panel = s_height == 32;
    if (animation % 3 == 0) {
        draw_smile(frame);
        return;
    }
    if (animation % 3 == 1) {
        circle(frame, 64, short_panel ? 24 : 49, short_panel ? 4 : 8);
        return;
    }
    int top = short_panel ? 20 : 41;
    int middle = short_panel ? 23 : 49;
    int bottom = short_panel ? 28 : 58;
    int left = short_panel ? 54 : 44;
    int right = short_panel ? 74 : 84;
    line(frame, left, middle, left + 5, top);
    line(frame, left + 5, top, right - 5, top);
    line(frame, right - 5, top, right, middle);
    line(frame, right, middle, right - 5, bottom);
    line(frame, right - 5, bottom, left + 5, bottom);
    line(frame, left + 5, bottom, left, middle);
}

static void draw_face(uint8_t *frame, mp_display_activity_t activity,
                      mp_display_mood_t mood, uint32_t animation, bool blink)
{
    bool short_panel = s_height == 32;
    memset(frame, 0, OLED_WIDTH * s_height / 8);
    if (activity == MP_DISPLAY_THINKING) {
        int shift = animation & 1U ? -3 : 2;
        draw_eyes(frame, blink, shift, short_panel ? -1 : -2);
        line(frame, 25, short_panel ? 1 : 10, 49, short_panel ? 0 : 9);
        int brow_top = short_panel ? 5 : 18;
        for (int y = 0; y < brow_top; y++) {
            for (int x = 79; x <= 103; x++) {
                frame[(y / 8) * OLED_WIDTH + x] &= ~(1U << (y & 7));
            }
        }
        line(frame, 79, short_panel ? 5 : 18, 103, short_panel ? 6 : 19);
        draw_smile(frame);
        return;
    }
    draw_eyes(frame, mood == MP_DISPLAY_SLEEPY || blink, 0, 0);
    if (mood == MP_DISPLAY_SAD) {
        line(frame, 25, short_panel ? 5 : 18, 49, short_panel ? 2 : 14);
        line(frame, 79, short_panel ? 2 : 14, 103, short_panel ? 5 : 18);
    } else if (mood == MP_DISPLAY_SURPRISED) {
        line(frame, 25, short_panel ? 1 : 11, 49, short_panel ? 1 : 11);
        line(frame, 79, short_panel ? 1 : 11, 103, short_panel ? 1 : 11);
    }
    if (activity == MP_DISPLAY_RESPONDING) {
        draw_speaking_mouth(frame, animation);
    } else {
        draw_smile(frame);
    }
}

static size_t draw_jobs(uint8_t *frame, size_t index, size_t count, size_t text_offset)
{
    memset(frame, 0, OLED_WIDTH * s_height / 8);
    if (!count) {
        draw_text(frame, 43, (s_height - 7) / 2, "NO JOBS");
        return 0;
    }
    mp_schedule_info_t info;
    char text[225];
    if (!mp_scheduler_get(index, &info, text, sizeof(text))) {
        return 0;
    }
    char header[33];
    snprintf(header, sizeof(header), "JOBS %u/%u", (unsigned)(index + 1), (unsigned)count);
    draw_text(frame, 1, 0, header);
    if (text_offset >= strlen(text)) {
        text_offset = 0;
    }
    if (text_offset) {
        size_t next = draw_wrapped(frame, 8, text + text_offset, s_height / 8 - 1);
        return next ? text_offset + next : 0;
    }
    struct tm local;
    time_t epoch = (time_t)info.next_epoch;
    localtime_r(&epoch, &local);
    char when[24];
    strftime(when, sizeof(when), "%d %b %H:%M", &local);
    snprintf(header, sizeof(header), "#%lu %s", (unsigned long)info.id, when);
    draw_text(frame, 1, 8, header);
    if (info.running) {
        strlcpy(header, "RUNNING", sizeof(header));
    } else if (!info.repeat_seconds) {
        strlcpy(header, "ONE TIME", sizeof(header));
    } else if (info.repeat_seconds % 86400 == 0) {
        snprintf(header, sizeof(header), "EVERY %luD",
                 (unsigned long)(info.repeat_seconds / 86400));
    } else if (info.repeat_seconds % 3600 == 0) {
        snprintf(header, sizeof(header), "EVERY %luH",
                 (unsigned long)(info.repeat_seconds / 3600));
    } else if (info.repeat_seconds % 60 == 0) {
        snprintf(header, sizeof(header), "EVERY %luM",
                 (unsigned long)(info.repeat_seconds / 60));
    } else {
        snprintf(header, sizeof(header), "EVERY %luS",
                 (unsigned long)info.repeat_seconds);
    }
    draw_text(frame, 1, 16, header);
    size_t next = draw_wrapped(frame, 24, text, s_height / 8 - 3);
    return next;
}

static button_event_t button_event(int64_t now, int *raw_level, int *stable_level,
                                   int64_t *changed_us, int64_t *pressed_us,
                                   bool *held, bool allow_hold)
{
    int level = gpio_get_level(OLED_BUTTON);
    if (level != *raw_level) {
        *raw_level = level;
        *changed_us = now;
    }
    if (level != *stable_level && now - *changed_us >= 40000) {
        *stable_level = level;
        if (level == 0) {
            *pressed_us = now;
            *held = false;
        } else if (*pressed_us && !*held) {
            *pressed_us = 0;
            return BUTTON_CLICK;
        }
    }
    if (allow_hold && *stable_level == 0 && *pressed_us && !*held &&
        now - *pressed_us >= 1200000) {
        *held = true;
        return BUTTON_HOLD;
    }
    return BUTTON_NONE;
}

static void display_task(void *argument)
{
    (void)argument;
    esp_err_t init_error = display_init();
    if (init_error != ESP_OK) {
        mp_metrics_error("display", init_error);
        s_enabled = false;
        mp_metrics_register("display", NULL);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_enabled = true;
    uint8_t frame[OLED_WIDTH * OLED_MAX_HEIGHT / 8];
    bool jobs = false;
    bool last_blink = false;
    bool first = true;
    bool failed = false;
    int raw_button = gpio_get_level(OLED_BUTTON);
    int stable_button = raw_button;
    int64_t button_changed = esp_timer_get_time();
    int64_t pressed_us = 0;
    bool held = false;
    size_t job_index = 0;
    size_t text_offset = 0;
    size_t next_offset = 0;
    size_t last_count = SIZE_MAX;
    mp_display_activity_t last_activity = MP_DISPLAY_IDLE;
    mp_display_mood_t last_mood = MP_DISPLAY_HAPPY;
    uint32_t last_animation = UINT32_MAX;
    while (true) {
        int64_t now = esp_timer_get_time();
        button_event_t event = button_event(now, &raw_button, &stable_button,
                                            &button_changed, &pressed_us, &held, true);
        if (event == BUTTON_HOLD) {
            if (jobs) {
                jobs = false;
                first = true;
                continue;
            }
            uint8_t off = 0xae;
            esp_err_t error = oled_commands(&off, 1);
            if (error != ESP_OK) {
                mp_metrics_error("display", error);
            }
            display_deinit();
            s_wake_ready = false;
            s_wake_pressed_us = 0;
            s_enabled = false;
            mp_metrics_register("display", NULL);
            s_task = NULL;
            vTaskDelete(NULL);
            return;
        }
        if (event == BUTTON_CLICK) {
            if (!jobs) {
                jobs = true;
                job_index = 0;
                text_offset = 0;
                next_offset = 0;
            } else {
                size_t count = mp_scheduler_count();
                if (next_offset) {
                    text_offset = next_offset;
                } else if (count) {
                    job_index = (job_index + 1) % count;
                    text_offset = 0;
                }
            }
            first = true;
        }
        esp_err_t error = ESP_OK;
        if (jobs) {
            size_t count = mp_scheduler_count();
            if (count && job_index >= count) {
                job_index = 0;
                text_offset = 0;
                next_offset = 0;
            }
            if (first || count != last_count) {
                if (count != last_count) {
                    text_offset = 0;
                }
                next_offset = draw_jobs(frame, job_index, count, text_offset);
                error = oled_write(frame);
                first = false;
                last_count = count;
            }
        } else {
            mp_display_activity_t activity = s_activity;
            mp_display_mood_t mood = s_mood;
            bool blink = now % 4000000 < 180000;
            uint32_t animation = (uint32_t)(now /
                (activity == MP_DISPLAY_RESPONDING ? 250000 : 500000));
            bool animated = activity != MP_DISPLAY_IDLE;
            if (first || activity != last_activity || mood != last_mood ||
                blink != last_blink ||
                (animated && animation != last_animation)) {
                draw_face(frame, activity, mood, animation, blink);
                error = oled_write(frame);
                first = false;
                last_activity = activity;
                last_mood = mood;
                last_blink = blink;
                last_animation = animation;
            }
        }
        if (error != ESP_OK && !failed) {
            failed = true;
            mp_metrics_error("display", error);
            ESP_LOGW(TAG, "write: %s", esp_err_to_name(error));
        } else if (error == ESP_OK) {
            failed = false;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
}

static esp_err_t display_task_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(display_task, "mp_display", CONFIG_MICROPAW_DISPLAY_STACK,
                                NULL, 2, &s_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    mp_metrics_register("display", s_task);
    return ESP_OK;
}

static void wake_timer_callback(void *argument)
{
    (void)argument;
    if (s_task) {
        return;
    }
    int level = gpio_get_level(OLED_BUTTON);
    int64_t now = esp_timer_get_time();
    if (level != 0) {
        s_wake_ready = true;
        s_wake_pressed_us = 0;
        return;
    }
    if (!s_wake_ready) {
        return;
    }
    if (!s_wake_pressed_us) {
        s_wake_pressed_us = now;
    } else if (now - s_wake_pressed_us >= 1200000) {
        s_wake_ready = false;
        s_wake_pressed_us = 0;
        esp_err_t error = display_task_start();
        if (error != ESP_OK) {
            mp_metrics_error("display", error);
        }
    }
}

esp_err_t mp_display_start(void)
{
    if (strcmp(mp_config_get()->oled_enabled, "true") != 0) {
        return ESP_OK;
    }
    s_height = (uint8_t)atoi(mp_config_get()->oled_height);
    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << OLED_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t error = gpio_config(&button_config);
    if (error != ESP_OK) {
        return error;
    }
    esp_timer_create_args_t timer_config = {
        .callback = wake_timer_callback,
        .name = "oled_button"
    };
    error = esp_timer_create(&timer_config, &s_wake_timer);
    if (error == ESP_OK) {
        error = esp_timer_start_periodic(s_wake_timer, 50000);
    }
    if (error != ESP_OK) {
        if (s_wake_timer) {
            esp_timer_delete(s_wake_timer);
            s_wake_timer = NULL;
        }
        return error;
    }
    s_wake_ready = gpio_get_level(OLED_BUTTON) != 0;
    error = display_task_start();
    if (error != ESP_OK) {
        esp_timer_stop(s_wake_timer);
        esp_timer_delete(s_wake_timer);
        s_wake_timer = NULL;
    }
    return error;
}

static void display_notify(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void mp_display_agent_begin(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_agent_busy = true;
    s_activity = MP_DISPLAY_THINKING;
    portEXIT_CRITICAL(&s_state_lock);
    display_notify();
}

void mp_display_agent_end(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_agent_busy = false;
    if (!s_response_pending) {
        s_activity = MP_DISPLAY_IDLE;
        s_mood = MP_DISPLAY_HAPPY;
    }
    portEXIT_CRITICAL(&s_state_lock);
    display_notify();
}

void mp_display_response_begin(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_response_pending = true;
    s_activity = MP_DISPLAY_RESPONDING;
    portEXIT_CRITICAL(&s_state_lock);
    display_notify();
}

void mp_display_response_end(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_response_pending = false;
    if (!s_agent_busy) {
        s_activity = MP_DISPLAY_IDLE;
        s_mood = MP_DISPLAY_HAPPY;
    }
    portEXIT_CRITICAL(&s_state_lock);
    display_notify();
}

void mp_display_filter_text(char *text)
{
    if (!text) {
        return;
    }
    char *start = text;
    while (*start == ' ' || *start == '\r' || *start == '\n' || *start == '\t') {
        start++;
    }
    static const struct {
        const char *tag;
        mp_display_mood_t mood;
    } moods[] = {
        {"[happy]", MP_DISPLAY_HAPPY},
        {"[sad]", MP_DISPLAY_SAD},
        {"[surprised]", MP_DISPLAY_SURPRISED},
        {"[sleepy]", MP_DISPLAY_SLEEPY}
    };
    for (size_t index = 0; index < sizeof(moods) / sizeof(moods[0]); index++) {
        size_t length = strlen(moods[index].tag);
        if (strncmp(start, moods[index].tag, length) != 0) {
            continue;
        }
        portENTER_CRITICAL(&s_state_lock);
        s_mood = moods[index].mood;
        portEXIT_CRITICAL(&s_state_lock);
        start += length;
        while (*start == ' ' || *start == '\r' || *start == '\n' || *start == '\t') {
            start++;
        }
        memmove(text, start, strlen(start) + 1);
        display_notify();
        return;
    }
}

bool mp_display_enabled(void)
{
    return s_enabled;
}
