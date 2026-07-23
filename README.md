<p align="center">
  <img src="assets/micropaw-logo.png" alt="MicroPaw logo" width="420">
</p>

<h1 align="center">MicroPaw</h1>

MicroPaw is a personal assistant that runs on an ESP32-S3. It supports Telegram, memory, scheduled reminders, web tools, Gmail and Google Calendar. The firmware uses ESP-IDF and C.

The supported target is an ESP32-S3 N16R8 with 8 MB octal PSRAM and 16 MB flash.

## Build

```sh
idf.py build
idf.py -p PORT flash
```

## Configure

Create `secrets/credentials.toml`. The directory is ignored by Git.

```toml
wifi_ssid = "NETWORK"
wifi_password = "PASSWORD"
telegram_token = "BOT_TOKEN"
owner_chat_id = "CHAT_ID"
llm_provider = "openrouter"
llm_api_key = "API_KEY"
llm_model = "openai/gpt-5.6-luna"
llm_max_output_tokens = "32768"
google_client_id = "CLIENT_ID"
google_client_secret = "CLIENT_SECRET"
google_refresh_token = "REFRESH_TOKEN"
email_permission = "permission"
calendar_permission = "permission"
timezone = "NZST-12NZDT,M9.5.0,M4.1.0/3"
```

Push it to the ESP and reboot:

```sh
python scripts/push_config.py PORT secrets/credentials.toml --reboot
```

The TOML file stays on the computer. `allowed` runs Gmail or Calendar actions immediately, `permission` asks through Telegram, and `disabled` turns the tool off.

The agent has no fixed tool-call or page-count limit. Gmail and Calendar listings return numbered pages of up to 20 records with a continuation token. The agent can keep paging until the task is done. Device request capacity and external API limits still apply.

OpenAI, OpenRouter and custom Responses-compatible HTTPS endpoints use the same streamed agent path. The default build allows 32,768 output tokens and 128 KB of reply or immediate-email text. Both values are configurable in Kconfig. Gmail uploads are base64url encoded while they are sent, so encoded and JSON copies are not kept in RAM. Scheduled emails are smaller because they must fit persistent NVS job storage.

Gmail supports numbered search and listing, message reads, send, scheduled send, read state, archive, inbox, star, spam, trash and restore. Calendar supports numbered listing and search, event details, create, partial update and delete. Permanent Gmail deletion is excluded. The narrower `gmail.modify` scope handles the implemented mailbox actions without granting immediate hard deletion.

Persistent memory holds eight durable entries of up to 1,023 bytes each. Separate facts should use separate entries.

Firmware builds are signed with the RSA-3072 key at `secrets/micropaw_signing_key.pem`. GitHub Actions reads the same key from `MICROPAW_SIGNING_KEY_B64`. Install the first signed image over USB before using OTA updates.

Google OAuth needs these scopes in one offline grant:

```text
https://www.googleapis.com/auth/gmail.modify
https://www.googleapis.com/auth/calendar.events.owned
```

An existing refresh token created with only `gmail.send` must be replaced after granting `gmail.modify`.

## Commands

`/help` lists Telegram commands.

`/metrics` reports request, inference, tool, context, delivery and HTTP timings without message content.

`/memory` lists saved facts, and `/jobs` lists scheduled jobs.

`/forget` clears conversation context while keeping saved facts and jobs.

`/reset` starts the owner-only two-step reset, and `/reset cancel` cancels it.

`/update` installs the latest signed release for this project and skips the current version.

`/confirm ID` approves a pending permission, and `/cancel ID` rejects it.

The serial console supports `config`, `set KEY VALUE`, `push-config BYTES`, `metrics`, `submit TEXT`, `reset-state YES`, `erase-config YES` and `reboot`.
