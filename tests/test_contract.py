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
        self.assertIn("short task-specific progress", prompt)
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
        self.assertIn("sha256sum", workflow)
        self.assertIn("gh release create", workflow)
        self.assertIn("--verify-tag", workflow)
        self.assertIn("write-flash 0x0", source("scripts/install.sh"))
        self.assertIn("Get-FileHash", source("scripts/install.ps1"))
        self.assertIn("CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y", defaults)
        self.assertIn("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y", defaults)


if __name__ == "__main__":
    unittest.main()
