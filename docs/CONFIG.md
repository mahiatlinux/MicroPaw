# Configuration

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
transcription_model = ""
personality = ""
google_client_id = "CLIENT_ID"
google_client_secret = "CLIENT_SECRET"
google_refresh_token = "REFRESH_TOKEN"
instagram_enabled = false
zernio_api_key = "API_KEY"
instagram_owner_username = "OWNER_USERNAME"
email_permission = "permission"
calendar_permission = "permission"
timezone = "NZST-12NZDT,M9.5.0,M4.1.0/3"
morning_briefing_enabled = false
morning_briefing_time = "08:00"
oled_enabled = false
oled_height = "64"
brave_api_key = "API_KEY"
brave_country = "NZ"
brave_search_lang = "en"
```

After the first flash, push the TOML file over USB. On macOS or Linux:

```sh
curl -fsSLO https://github.com/mahiatlinux/MicroPaw/releases/latest/download/push_config.py && python3 push_config.py secrets/credentials.toml --reboot
```

In PowerShell:

```powershell
$push = [scriptblock]::Create((irm https://github.com/mahiatlinux/MicroPaw/releases/latest/download/push_config.ps1)); & $push .\secrets\credentials.toml -Reboot
```

The uploaders use only Python's standard library on macOS and Linux, and built-in PowerShell APIs on Windows. They find the port when one matching device is connected. Use `--port PORT` or `-Port PORT` when needed. The TOML file stays on the computer.

## Brave search

Web search uses the [Brave Search API](https://api.search.brave.com/app/documentation/web-search/get-started). Get a key:

1. Create an account at [api.search.brave.com](https://api.search.brave.com/app/keys)
2. Subscribe to Search. It costs $5 per 1,000 requests, includes $5 in monthly credit and requires a payment card for account verification
3. Copy the API key into `brave_api_key`
4. Set `brave_country` to a two-letter country code for local ranking, or leave it empty
5. Set `brave_search_lang` to the result language
6. Push the TOML again and reboot

Without a key the `web_search` tool returns an error. Wikipedia and arXiv providers do not need a key.

## Permission modes

`allowed` runs Gmail or Calendar actions immediately. `permission` asks through Telegram. `disabled` turns the tool off.

## Instagram

Instagram through [Zernio](https://zernio.com/) is optional and disabled by default. Telegram-only installs do not need a Zernio account.

To enable:

1. Connect an Instagram Business or Creator account to Zernio
2. Create a read-write API key limited to that profile
3. Send the connected account one DM from the owner account so Zernio can see the conversation
4. Set `instagram_enabled = true`, add the key, and set `instagram_owner_username` to the owner's username without `@`
5. Push the TOML again and reboot

The first matching DM binds that username to its Instagram participant ID on the device. Other accounts are ignored.

Instagram uses the same Luna context, progress messages, tools, permissions and scheduled-job delivery as Telegram. Instagram is slower. Zernio adds another service in both directions, and the ESP must poll its 60-request-per-minute free API. Replies can take several seconds longer. A Zernio delay or outage also stops Instagram delivery. Telegram is the recommended primary interface.

Telegram photos up to 2 MiB go into Luna's normal turn with their caption. The ESP keeps one raw image in PSRAM and base64 encodes it while sending the request. Instagram image attachments use Zernio's public HTTPS URLs. Several images can share one DM if their packed URLs fit the device's 1,024-byte reference area. Image bytes and URLs are discarded after the turn.

## Voice transcription

Telegram voice notes up to 2 MiB are transcribed before Luna sees the message. OpenAI defaults to `gpt-4o-mini-transcribe`. OpenRouter defaults to `openai/gpt-4o-mini-transcribe`. Set `transcription_model` to replace that default. Voice notes are rejected with a clear reply when `llm_provider = "openai_compatible"` because a transcription endpoint cannot be derived from a custom Responses URL. Audio is discarded after transcription.

## Personality

Set `personality` to one printable line of up to 768 bytes. Luna receives it in chat, reminder and morning-briefing instructions. Context compaction does not receive it.

## Morning briefing

The morning briefing runs once per local day at `morning_briefing_time`. It checks today's calendar, useful unread Gmail and upcoming reminders, skipping services set to `disabled`. A boot within four hours of the configured time catches up. A later boot records that day as skipped. Briefing state lives outside the eight reminder slots.

## OLED display

Set `oled_enabled = true` for a 128×32 or 128×64 SSD1306 I2C OLED wired to SDA GPIO8 and SCL GPIO9. Set `oled_height` to match the panel.

Click BOOT on the face to open jobs, then click to move through their pages. Hold BOOT on the jobs screen to return to the face. Hold it on the face to turn the OLED off, then hold it again to restart it.

## Google OAuth

Google OAuth needs these scopes in one offline grant:

```text
https://www.googleapis.com/auth/gmail.modify
https://www.googleapis.com/auth/calendar.events.owned
```

An existing refresh token created with only `gmail.send` must be replaced after granting `gmail.modify`.
