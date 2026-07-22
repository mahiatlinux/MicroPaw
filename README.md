<p align="center">
  <img src="assets/micropaw-logo.png" alt="MicroPaw logo" width="420">
</p>

<h1 align="center">MicroPaw</h1>

MicroPaw keeps its agent loop, tools, memory, schedules and channel routing on an ESP32. Remote services supply inference and requested data. They do not own the assistant state.

The current build targets the detected ESP32-S3 revision 0.2 with 16 MB flash, 8 MB octal PSRAM, a 40 MHz crystal and native USB Serial/JTAG. It uses ESP-IDF 6.0.2 and C only.

## Included features

- Fixed-queue agent state machine with bounded Responses API streaming and serial tool execution
- Telegram long polling with one configured owner chat
- NVS-backed durable facts, recent conversation records and eight scheduled jobs
- One-time and repeating reminders with proactive Telegram delivery
- Independent `allowed`, `permission` and `disabled` modes for email and Calendar
- Direct DuckDuckGo Lite search with a small streaming HTML state machine
- Replaceable search interface plus Wikipedia and arXiv providers
- Direct, bounded page fetching with incremental HTML text extraction
- Bounded RSS and Atom parsing
- One shared HTTPS mutex, explicit connection lifetime and CA bundle validation
- Bounded TOML configuration pushes over USB serial with one NVS commit
- Build-time removal of Telegram, search, page fetch, Wikipedia, arXiv, RSS, Gmail and Calendar
- Static task stacks, fixed work buffers and live heap and stack measurements

DuckDuckGo Lite is a browser endpoint, not a guaranteed public machine API. Its markup, access policy and availability can change. The provider has passed a desktop response probe, but it has not been tested repeatedly on this board and is not claimed to be reliable.

OpenAlex is not included. Its current API requires a key and a metered account, which does not meet the no-key source requirement.

## Build and flash

Open an ESP-IDF 6.0.2 shell in the project directory, then run:

```powershell
idf.py build
idf.py -p COM3 flash
python scripts/runtime_report.py COM3
```

The selected board uses USB Serial/JTAG for the console. A different ESP32-S3 board may need flash, PSRAM and console changes in `sdkconfig.defaults`.

## Configure without committed secrets

Create `secrets/credentials.toml`; the `secrets/` directory is ignored by Git:

```toml
wifi_ssid = "NETWORK"
wifi_password = "PASSWORD"
telegram_token = "BOT_TOKEN"
owner_chat_id = "NUMERIC_CHAT_ID"
llm_provider = "openai"
llm_api_key = "API_KEY"
llm_model = "gpt-5.6-luna"
email_permission = "permission"
calendar_permission = "permission"
timezone = "NZST-12NZDT,M9.5.0,M4.1.0/3"
```

Push it over USB serial and reboot:

```powershell
python scripts/push_config.py COM3 secrets/credentials.toml --reboot
```

The file may contain any subset of the keys shown by `config`. MicroPaw accepts top-level keys with single-line quoted string values and comments. Double-quoted values support `\"` and `\\`; single-quoted values preserve text literally. It rejects unknown keys, duplicates, tables, arrays and oversized values before committing the document to NVS. The maximum file size is 4096 bytes.

Values can also be set individually. Open a 115200 baud serial terminal and enter one setting per line:

```text
set wifi_ssid NETWORK
set wifi_password PASSWORD
set telegram_token BOT_TOKEN
set owner_chat_id NUMERIC_CHAT_ID
set llm_provider openai
set llm_api_key API_KEY
set llm_model gpt-5.6-luna
set email_permission permission
set calendar_permission permission
set timezone NZST-12NZDT,M9.5.0,M4.1.0/3
reboot
```

`config` reports which secrets are set without printing their values. `push-config BYTES` is the bounded serial protocol used by the push script. `erase-config YES` removes every stored configuration value. `metrics` prints heap minima, largest blocks and active-task stack watermarks. `submit TEXT` sends a local test request to the agent queue. LLM provider and permission changes apply to the next request. Reboot after changing Wi-Fi, Telegram or timezone values.

## LLM providers

`llm_provider` accepts `openai`, `openrouter` or `openai_compatible`. One active `llm_api_key` keeps credential handling small. Change it when switching accounts or providers.

OpenAI uses the fixed official `https://api.openai.com/v1/responses` endpoint:

```text
set llm_provider openai
set llm_api_key OPENAI_KEY
set llm_model gpt-5.6-luna
```

OpenRouter uses its documented beta Responses endpoint. Model IDs include the organisation prefix and must support tool calling:

```text
set llm_provider openrouter
set llm_api_key OPENROUTER_KEY
set llm_model openai/o4-mini
```

OpenRouter Responses is stateless. MicroPaw therefore includes the bounded current-turn function calls and outputs in each follow-up request. The endpoint is beta and may change.

A custom endpoint must implement the OpenAI Responses streaming and function-calling contract. Chat Completions compatibility alone is insufficient. Supply the complete HTTPS endpoint, including `/responses` if the service uses that path:

```text
set llm_provider openai_compatible
set llm_endpoint https://llm.example.com/v1/responses
set llm_api_key OPTIONAL_BEARER_KEY
set llm_model MODEL_ID
```

The key may be empty for a custom endpoint. Plain HTTP and private self-signed certificates are not supported; TLS validation uses the ESP certificate bundle. MicroPaw sends standard bearer authentication when a key is present.

Development builds store configuration in ordinary NVS. For a production device, review ESP-IDF flash-encryption consequences first, then build with:

```powershell
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secure" build
```

Flash encryption can change eFuses and device recovery behaviour. Do not enable it as a casual development setting.

## Google tools

Gmail and Calendar are enabled by default. Enable both APIs in the Google Cloud project, then obtain offline OAuth consent for these scopes in one grant:

```text
https://www.googleapis.com/auth/gmail.send
https://www.googleapis.com/auth/calendar.events.owned
```

Provision the resulting values over serial:

```text
set google_client_id CLIENT_ID
set google_client_secret CLIENT_SECRET
set google_refresh_token REFRESH_TOKEN
set email_permission permission
set calendar_permission permission
```

The device refreshes the access token directly at Google. Gmail uses `users.messages.send` and Calendar uses `events.insert`. Each tool has its own permission mode. `allowed` executes immediately, `permission` waits up to five minutes for `/confirm ID`, and `disabled` hides and blocks the tool. `/cancel ID` rejects a pending action. The default is `permission`.

## Commands and tools

Telegram commands are `/help`, `/metrics`, `/memory`, `/jobs`, `/forget`, `/confirm ID` and `/cancel ID`.

The model can call `memory_save`, `memory_list`, `schedule_add`, `schedule_list`, `schedule_delete`, `time_now`, `diagnostics`, `web_search`, `web_fetch` and `rss_read`. `email_send` and `calendar_create` appear unless their permission is `disabled`.

Search returns at most five titles, URLs and snippets. `web_fetch` accepts only a URL from the latest result set, follows at most one public HTTPS redirect, checks content type, stops at the configured byte limit and returns bounded visible text.

## Reports and layout

Run `scripts/report.ps1 -Port COM3` from an exported ESP-IDF shell. It writes build, size, component and live runtime reports under `reports/`. Generated reports are ignored by Git.

- `prompts/` contains the system and scheduled-turn prompts as separate source files.
- `components/micropaw_agent/` contains the state machine and inference client.
- `components/micropaw_base/` contains configuration, memory, scheduling, permission state, JSON slices and metrics.
- `components/micropaw_net/` owns Wi-Fi, time sync and all HTTPS connections.
- `components/micropaw_tools/` contains the static registry and service integrations.
- `components/micropaw_telegram/` contains the first channel adapter.
- `RESEARCH.md` records reviewed revisions, official contracts and benchmark qualifications.

## Current test limits

Build, flash, PSRAM startup, NVS defaults, scheduler startup and the original serial commands have been tested on the connected board. The TOML push and permission modes are not yet hardware-tested. Wi-Fi, Telegram, OpenAI, OpenRouter, custom inference, DuckDuckGo, specialised searches, page fetching and Google writes have not been exercised on-board because no user credentials or network configuration were provisioned. The measured build predates Gmail and Calendar being enabled by default.
