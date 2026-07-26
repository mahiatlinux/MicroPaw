<p align="center">
  <img src="assets/micropaw-logo.png" alt="MicroPaw logo" width="420">
</p>

<h1 align="center">MicroPaw</h1>

Personal assistant firmware for ESP32-S3. Runs GPT-5.6 Luna on the device with Telegram, memory, scheduled reminders, web search, Gmail and Google Calendar.

**Supported hardware:** ESP32-S3 N16R8 with 8 MB octal PSRAM and 16 MB flash.

## Features

- LLM agent with tool calling (OpenAI, OpenRouter or custom Responses endpoint)
- Telegram and optional Instagram messaging
- DuckDuckGo search, Wikipedia, arXiv, RSS/Atom readers
- Gmail (read, send, schedule, labels) and Google Calendar (list, create, update, delete)
- Voice note transcription and photo input
- Persistent memory (8 entries) and scheduled reminders (8 jobs)
- Morning briefing
- Optional SSD1306 OLED display
- OTA updates with signed releases

## Install

Connect the ESP32-S3 over USB. On macOS or Linux:

```sh
curl -fsSL https://github.com/mahiatlinux/MicroPaw/releases/latest/download/install.sh | sh
```

In PowerShell:

```powershell
irm https://github.com/mahiatlinux/MicroPaw/releases/latest/download/install.ps1 | iex
```

The installer needs Python 3.10 or newer. Set `MICROPAW_PORT` when more than one serial device is connected. If automatic reset fails, hold BOOT while the installer starts.

## Configure

Create `secrets/credentials.toml`:

```toml
wifi_ssid = "NETWORK"
wifi_password = "PASSWORD"
telegram_token = "BOT_TOKEN"
owner_chat_id = "CHAT_ID"
llm_provider = "openrouter"
llm_api_key = "API_KEY"
llm_model = "openai/gpt-5.6-luna"
```

Push the TOML file over USB after the first flash. See [docs/CONFIG.md](docs/CONFIG.md) for the full settings reference including Instagram, OLED, morning briefing, voice transcription and Google OAuth setup.

## Build

```sh
idf.py build
idf.py -p PORT flash
```

## Commands

Run `/help` on the device for available commands. See [docs/COMMANDS.md](docs/COMMANDS.md) for the full listing.
