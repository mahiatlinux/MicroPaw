from pathlib import Path
import csv
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text()


class FirmwareContractTest(unittest.TestCase):
    def test_capacity_contract(self):
        types = source("components/micropaw_base/include/mp_types.h")
        defaults = source("sdkconfig.defaults")
        kconfig = source("main/Kconfig.projbuild")
        metrics = source("components/micropaw_base/mp_metrics.c")
        expected = [
            "#define MP_MESSAGE_LEN 1024",
            "#define MP_TOOL_RESULT_LEN 65536",
            "#define MP_AGENT_REQUEST_LEN (CONFIG_MICROPAW_WORK_TEXT_BYTES * 6 + 65536)",
            "#define MP_TOOL_TRACE_LEN (CONFIG_MICROPAW_WORK_TEXT_BYTES * 4 + 32768)",
            "#define MP_MEMORY_SLOTS 8",
            "#define MP_SCHEDULE_SLOTS 8",
        ]
        for value in expected:
            self.assertIn(value, types)
        self.assertIn("CONFIG_MICROPAW_WORK_TEXT_BYTES=131072", defaults)
        self.assertIn("CONFIG_MICROPAW_LLM_MAX_OUTPUT_TOKENS=32768", defaults)
        self.assertIn("CONFIG_MICROPAW_LLM_STREAM_LIMIT=4194304", defaults)
        self.assertIn("CONFIG_MICROPAW_PAGE_DOWNLOAD_LIMIT=131072", defaults)
        self.assertIn("default 49152", kconfig)
        for name in [
            "google_response_capacity",
            "page_download_capacity",
            "search_response_capacity",
            "incoming_text_capacity",
            "email_body_capacity",
            "decoded_email_capacity",
            "page_records",
        ]:
            self.assertIn(name, metrics)

    def test_partition_contract(self):
        rows = []
        with (ROOT / "partitions.csv").open(newline="") as handle:
            for row in csv.reader(handle):
                if row and not row[0].lstrip().startswith("#"):
                    rows.append(tuple(value.strip() for value in row[:5]))
        self.assertEqual(rows, [
            ("nvs", "data", "nvs", "0x9000", "0x10000"),
            ("nvs_keys", "data", "nvs_keys", "0x19000", "0x1000"),
            ("phy_init", "data", "phy", "0x1A000", "0x1000"),
            ("otadata", "data", "ota", "0x1B000", "0x2000"),
            ("ota_0", "app", "ota_0", "0x20000", "0x200000"),
            ("ota_1", "app", "ota_1", "0x220000", "0x200000"),
            ("context", "data", "nvs", "0x420000", "0x100000"),
        ])

    def test_agent_batch_and_progress_contract(self):
        agent = source("components/micropaw_agent/mp_agent.c")
        llm = source("components/micropaw_agent/mp_llm.c")
        prompt = source("prompts/system.txt")
        self.assertIn('\\"parallel_tool_calls\\":true', agent)
        self.assertIn("while ((call = mp_llm_call_next", agent)
        self.assertIn("append_tool_exchange(call, s_tool_output)", agent)
        self.assertIn("send_progress(message->chat_id, s_result.text)", agent)
        self.assertIn("progress_fallback(&s_result)", agent)
        self.assertIn("batch_needs_progress(&s_result)", agent)
        self.assertIn("current_reads & write_bit", agent)
        self.assertIn("Read the current matching state", agent)
        self.assertIn("return \"I'll search the web for that.\"", agent)
        self.assertIn("short, natural, task-specific progress", prompt)
        self.assertIn("Omit progress for quick local actions", prompt)
        self.assertIn("read the current matching state in the same turn", prompt)
        self.assertIn("mp_llm_parse_chunk", llm)
        self.assertIn("call->order > *offset", llm)

    def test_context_and_reset_contract(self):
        context = source("components/micropaw_base/include/mp_context.h")
        implementation = source("components/micropaw_base/mp_context.c")
        agent = source("components/micropaw_agent/mp_agent.c")
        telegram = source("components/micropaw_telegram/mp_telegram.c")
        self.assertIn("#define MP_CONTEXT_TURN_TRIGGER 6", context)
        self.assertIn("#define MP_CONTEXT_BYTE_TRIGGER 131072", context)
        self.assertIn("#define MP_CONTEXT_BYTE_TARGET 65536", context)
        self.assertIn("#define MP_CONTEXT_SUMMARY_TEXT_LEN 32768", context)
        self.assertIn("return s_meta.last_compaction_ms", implementation)
        commit = implementation.index("error = write_meta(handle, &next)", 10000)
        erase = implementation.index("nvs_erase_key(handle, key)", commit)
        self.assertLess(commit, erase)
        self.assertIn("mp_context_last_compaction_ms()", source("components/micropaw_base/mp_metrics.c"))
        self.assertIn('strcmp(message->text, "/reset confirm YES")', agent)
        self.assertIn("300000000LL", agent)
        self.assertNotIn("/reset-state YES", agent)
        self.assertNotIn("/reset-state YES", telegram)

    def test_network_and_google_contract(self):
        net = source("components/micropaw_net/mp_net.c")
        google = source("components/micropaw_tools/mp_google.c")
        gmail = source("components/micropaw_tools/mp_gmail.c")
        calendar = source("components/micropaw_tools/mp_calendar.c")
        self.assertIn(".keep_alive_enable = true", net)
        self.assertIn("esp_http_client_prepare", net)
        self.assertIn("now + 60 < s_token_expiry", google)
        self.assertIn("https://gmail.googleapis.com/batch", gmail)
        self.assertIn("fields=nextPageToken,messages(id)", gmail)
        self.assertIn("<response-mp-%lu>", gmail)
        self.assertIn("id,summary,start,end,status", calendar)
        self.assertIn("EXT_RAM_BSS_ATTR static char s_encode_buffer[2048]", gmail)
        self.assertIn("base64url_size(raw_size) + 10, email_write", gmail)

    def test_telegram_and_ota_contract(self):
        telegram = source("components/micropaw_telegram/mp_telegram.c")
        ota = source("components/micropaw_ota/mp_ota.c")
        workflow = source(".github/workflows/release.yml")
        defaults = source("sdkconfig.defaults")
        self.assertIn("#define OUTBOUND_QUEUE_LENGTH 4", telegram)
        self.assertIn("char text[3901]", telegram)
        self.assertIn("pdMS_TO_TICKS(4000)", telegram)
        self.assertIn(".buffer_size = 1024", telegram)
        self.assertIn("releases/latest/download/micropaw.bin", ota)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", ota)
        self.assertIn("esp_ota_mark_app_valid_cancel_rollback", ota)
        self.assertIn("espressif/idf:v6.0.2", workflow)
        self.assertIn("MICROPAW_SIGNING_KEY_B64", workflow)
        self.assertIn("espsecure verify-signature", workflow)
        self.assertIn("micropaw-usb.hex", workflow)
        self.assertIn("push_config.py", workflow)
        self.assertIn("push_config.ps1", workflow)
        self.assertIn("sha256sum", workflow)
        self.assertIn("gh release create", workflow)
        self.assertIn("--verify-tag", workflow)
        self.assertIn("write-flash 0x0", source("scripts/install.sh"))
        self.assertIn("Get-FileHash", source("scripts/install.ps1"))
        self.assertNotIn("import serial", source("scripts/push_config.py"))
        self.assertIn("CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y", defaults)
        self.assertIn("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y", defaults)

    def test_optional_oled_contract(self):
        config = source("components/micropaw_base/mp_config.c")
        display = source("components/micropaw_display/mp_display.c")
        agent = source("components/micropaw_agent/mp_agent.c")
        telegram = source("components/micropaw_telegram/mp_telegram.c")
        scheduler = source("components/micropaw_base/mp_scheduler.c")
        prompt = source("prompts/system.txt")
        self.assertIn('{"oled_enabled", "oled"', config)
        self.assertIn('{"oled_height", "oled_h"', config)
        self.assertIn('strlcpy(s_config.oled_enabled, "false"', config)
        self.assertIn('strlcpy(s_config.oled_height, "64"', config)
        self.assertIn('strncmp(value, "true", 4)', config)
        self.assertIn("#define OLED_SDA GPIO_NUM_8", display)
        self.assertIn("#define OLED_SCL GPIO_NUM_9", display)
        self.assertIn("#define OLED_BUTTON GPIO_NUM_0", display)
        self.assertIn("#define OLED_ADDRESS 0x3c", display)
        self.assertIn('#include "font5x7.h"', display)
        self.assertIn('strcmp(mp_config_get()->oled_enabled, "true")', display)
        self.assertIn("xTaskCreatePinnedToCore(display_task", display)
        self.assertIn("esp_timer_start_periodic(s_wake_timer, 50000)", display)
        self.assertIn("event == BUTTON_HOLD", display)
        self.assertIn("event == BUTTON_CLICK", display)
        self.assertIn("text_offset = next_offset", display)
        self.assertIn("job_index = (job_index + 1) % count", display)
        self.assertIn('mp_metrics_register("display", NULL)', display)
        self.assertIn("s_mood = MP_DISPLAY_HAPPY", display)
        self.assertIn('{"[sad]", MP_DISPLAY_SAD}', display)
        self.assertIn("memmove(text, start, strlen(start) + 1)", display)
        self.assertIn("mp_display_filter_text(s_result.text)", agent)
        self.assertIn("mp_display_response_end()", telegram)
        self.assertIn("mp_display_agent_begin()", agent)
        self.assertIn("mp_display_agent_end()", agent)
        self.assertIn("mp_scheduler_get(size_t index", scheduler)
        self.assertIn("[happy], [sad], [surprised] or [sleepy]", prompt)

    def test_optional_instagram_contract(self):
        config = source("components/micropaw_base/mp_config.c")
        instagram = source("components/micropaw_instagram/mp_instagram.c")
        agent = source("components/micropaw_agent/mp_agent.c")
        main = source("main/main.c")
        readme = source("README.md")
        defaults = source("sdkconfig.defaults")
        self.assertIn('{"instagram_enabled", "instagram"', config)
        self.assertIn('{"zernio_api_key", "zernio_key"', config)
        self.assertIn('{"instagram_owner_username", "ig_owner"', config)
        self.assertIn('strlcpy(s_config.instagram_enabled, "false"', config)
        self.assertIn("#define OUTBOUND_QUEUE_LENGTH 4", instagram)
        self.assertIn("char text[OUTBOUND_TEXT_LEN]", instagram)
        self.assertIn("#define POLL_INTERVAL_MS 1250", instagram)
        self.assertNotIn("/read", instagram)
        self.assertEqual(instagram.count("static mp_http_session_t"), 1)
        self.assertIn("s_http_mutex", instagram)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", instagram)
        self.assertIn("MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT", instagram)
        self.assertNotIn("static StackType_t s_task_stack[", instagram)
        start = instagram.index("esp_err_t mp_instagram_start(void)\n{")
        self.assertLess(instagram.index('strcmp(config->instagram_enabled', start),
                        instagram.index("allocate_memory();", start))
        self.assertIn("xTaskCreateStaticPinnedToCore", instagram)
        self.assertIn("participantUsername", instagram)
        self.assertIn("participantId", instagram)
        self.assertIn('nvs_set_str(handle, "participant"', instagram)
        self.assertIn("mp_agent_submit_wait(chat_id, s_memory->message, false)", instagram)
        self.assertLess(instagram.index("mp_instagram_flush(portMAX_DELAY)"),
                        instagram.index("checkpoint_save(message_id)", instagram.index("mp_agent_submit_wait")))
        self.assertIn("mp_instagram_chat(chat_id)", main)
        self.assertIn("flush_output(const char *chat_id", main)
        self.assertIn("Instagram through [Zernio]", readme)
        self.assertIn("optional and disabled by default", readme)
        self.assertIn('strncmp(message->chat_id, "ig:", 3)', agent)
        self.assertIn("CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y", defaults)

    def test_offline_delivery_contract(self):
        agent = source("components/micropaw_agent/mp_agent.c")
        net = source("components/micropaw_net/mp_net.c")
        scheduler = source("components/micropaw_base/mp_scheduler.c")
        telegram = source("components/micropaw_telegram/mp_telegram.c")
        tools = source("components/micropaw_tools/mp_tools.c")
        self.assertIn("deliver_text(s_current.chat_id", telegram)
        self.assertIn("while (true)", telegram[telegram.index("static esp_err_t deliver_text"):])
        self.assertIn("mp_http_retryable(error)", telegram)
        self.assertIn("mp_agent_submit(id, s_message, false, portMAX_DELAY)", telegram)
        self.assertIn("return bits & OUTBOUND_IDLE ? error", telegram)
        self.assertIn("static uint32_t s_pending", telegram)
        self.assertIn("pending_complete();", telegram)
        self.assertNotIn("xSemaphoreTake(s_enqueue_mutex, portMAX_DELAY) == pdTRUE) {\n                if (uxQueueMessagesWaiting", telegram)
        self.assertIn("close_connection(session);", net)
        self.assertIn("error = esp_http_client_open(session->handle", net)
        self.assertIn("s_inflight[index] = true", scheduler)
        self.assertIn("mp_scheduler_complete(uint32_t id, bool success)", scheduler)
        self.assertIn("mp_scheduler_complete(message.schedule_id, success)", agent)
        self.assertIn("delivery = s_flush(message->chat_id, portMAX_DELAY)", agent)
        self.assertIn("scheduled for %s", tools)


if __name__ == "__main__":
    unittest.main()
