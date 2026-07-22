<p align="center">
  <img src="assets/micropaw-logo.png" alt="MicroPaw logo" width="420">
</p>

<h1 align="center">MicroPaw</h1>

MicroPaw is a personal assistant that runs on an ESP32-S3. It supports Telegram, memory, scheduled reminders, web tools, Gmail and Google Calendar. The firmware uses ESP-IDF and C.

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

The agent can run eight sequential tool calls per turn. Kconfig can set the limit from 1 to 12.

Google OAuth needs these scopes in one offline grant:

```text
https://www.googleapis.com/auth/gmail.send
https://www.googleapis.com/auth/calendar.events.owned
```

## Commands

Telegram supports `/help`, `/metrics`, `/memory`, `/jobs`, `/forget`, `/reset-state YES`, `/confirm ID` and `/cancel ID`.

`/reset-state YES` clears saved memory, conversation history, scheduled jobs and pending permissions. Credentials stay on the ESP.

The serial console supports `config`, `set KEY VALUE`, `push-config BYTES`, `metrics`, `submit TEXT`, `reset-state YES`, `erase-config YES` and `reboot`.
