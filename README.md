<p align="center">
  <img src="assets/micropaw-logo.png" alt="MicroPaw logo" width="420">
</p>

<h1 align="center">MicroPaw</h1>

MicroPaw is a personal assistant that runs on an ESP32-S3. It supports Telegram, memory, scheduled reminders, web tools, Gmail and Google Calendar. The firmware uses ESP-IDF and C.

The supported target is an ESP32-S3 N16R8 with 8 MB octal PSRAM and 16 MB flash.

## Install

Connect the ESP32-S3 over USB. On macOS or Linux, run:

```sh
curl -fsSL https://github.com/mahiatlinux/MicroPaw/releases/latest/download/install.sh | sh
```

In PowerShell, run:

```powershell
irm https://github.com/mahiatlinux/MicroPaw/releases/latest/download/install.ps1 | iex
```

The installer needs Python 3.10 or newer. It downloads the latest signed release, checks its SHA-256 hash, runs esptool in a temporary environment, finds the connected ESP32-S3 and flashes it. Existing credentials, memory, jobs and conversation context stay intact. Set `MICROPAW_PORT` when more than one serial device is connected. If automatic reset fails, hold BOOT while the installer starts.

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
instagram_enabled = false
zernio_api_key = "API_KEY"
instagram_owner_username = "OWNER_USERNAME"
email_permission = "permission"
calendar_permission = "permission"
timezone = "NZST-12NZDT,M9.5.0,M4.1.0/3"
oled_enabled = false
oled_height = "64"
```

After the first flash, download the configuration tool and push the TOML file over USB. On macOS or Linux:

```sh
curl -fsSLO https://github.com/mahiatlinux/MicroPaw/releases/latest/download/push_config.py && python3 push_config.py secrets/credentials.toml --reboot
```

In PowerShell:

```powershell
$push = [scriptblock]::Create((irm https://github.com/mahiatlinux/MicroPaw/releases/latest/download/push_config.ps1)); & $push .\secrets\credentials.toml -Reboot
```

The uploaders use only Python's standard library on macOS and Linux, and built-in PowerShell APIs on Windows. They find the port when one matching device is connected; use `--port PORT` or `-Port PORT` when needed. The TOML file stays on the computer. `allowed` runs Gmail or Calendar actions immediately, `permission` asks through Telegram, and `disabled` turns the tool off.

Instagram through [Zernio](https://zernio.com/) is optional and disabled by default. Telegram-only installs do not need a Zernio account. To turn it on, connect an Instagram Business or Creator account to Zernio and create a read-write API key limited to that profile. Send the connected account one DM from the owner account so Zernio can see the conversation. Set `instagram_enabled = true`, add the key, and set `instagram_owner_username` to the owner's username without `@`. Push the TOML again and reboot.

The first matching DM binds that username to its Instagram participant ID on the device. Other accounts are ignored. Instagram uses the same Luna context, progress messages, tools, permissions and scheduled-job delivery as Telegram. Instagram does not expose a typing action through this API, so the progress message is the first visible response on longer work.

Instagram is slower. Zernio adds another service in both directions, and the ESP must poll its 60-request-per-minute free API. Replies can take several seconds longer. A Zernio delay or outage also stops Instagram delivery, so Telegram is the recommended primary interface.

Set `oled_enabled = true` for a 128×32 or 128×64 SSD1306 I2C OLED wired to SDA GPIO8 and SCL GPIO9. Set `oled_height` to match the panel. Click BOOT on the face to open jobs, then click to move through their pages. Hold BOOT on the jobs screen to return to the face. Hold it on the face to turn the OLED off, then hold it again to restart it.

The agent has no fixed tool-call or page-count limit. Gmail and Calendar listings return numbered pages of up to 20 records with a continuation token. The agent can keep paging until the task is done. Device request capacity and external API limits still apply.

OpenAI, OpenRouter and custom Responses-compatible HTTPS endpoints use the same streamed agent path. The default build allows 32,768 output tokens and 128 KB of reply or immediate-email text. Both values are configurable in Kconfig. Gmail uploads are base64url encoded while they are sent, so encoded and JSON copies are not kept in RAM. Scheduled emails are smaller because they must fit persistent NVS job storage.

Gmail supports numbered search and listing, message reads, send, scheduled send, read state, archive, inbox, star, spam, trash and restore. Calendar supports numbered listing and search, event details, create, partial update and delete. Permanent Gmail deletion is excluded. The narrower `gmail.modify` scope handles the implemented mailbox actions without granting immediate hard deletion.

Persistent memory holds eight durable entries of up to 1,023 bytes each. Separate facts should use separate entries.

Firmware builds are signed with the RSA-3072 key at `secrets/micropaw_signing_key.pem`. GitHub Actions reads the same key from `MICROPAW_SIGNING_KEY_B64`. Each versioned release includes the OTA image, sparse USB image, installers and SHA-256 checksums.

Google OAuth needs these scopes in one offline grant:

```text
https://www.googleapis.com/auth/gmail.modify
https://www.googleapis.com/auth/calendar.events.owned
```

An existing refresh token created with only `gmail.send` must be replaced after granting `gmail.modify`.

## Commands

`/help` lists chat commands.

`/metrics` reports request, inference, tool, context, delivery and HTTP timings without message content.

`/memory` lists saved facts, and `/jobs` lists scheduled jobs.

`/forget` clears conversation context while keeping saved facts and jobs.

`/reset` starts the owner-only two-step reset, and `/reset cancel` cancels it.

`/update` installs the latest signed release for this project and skips the current version.

`/confirm ID` approves a pending permission, and `/cancel ID` rejects it.

The serial console supports `config`, `set KEY VALUE`, `push-config BYTES`, `metrics`, `submit TEXT`, `reset-state YES`, `erase-config YES` and `reboot`.
