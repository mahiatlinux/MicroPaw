# MicroPaw research

Research date: 2026-07-21

## Source projects

| Project | Revision reviewed | Licence | Relevant findings |
| --- | --- | --- | --- |
| [OpenClaw](https://github.com/openclaw/openclaw) | `e8842ddcedaba06e19f862528399421b245bbeab` | MIT | Serializes runs per session, separates channel intake from agent execution, bounds tool results, persists transcripts, supports scheduled turns, and applies tool and sender policies. Its Node gateway, plugin graph and host-level tools are unsuitable for an MCU. |
| [zclaw](https://github.com/tnm/zclaw) | `e3ad271244c1f2e01f4df6e81c2bab346e95d1b1` | MIT | Uses fixed FreeRTOS queues, a single agent task, NVS memory and schedules, Telegram allowlists, bounded tool rounds, a shared HTTPS mutex on constrained targets, rate limits and optional encrypted storage. The published default classic ESP32 image is 853,034 bytes, with 39,276 bytes attributed to application logic. Direct search and page fetching are absent. |
| [MimiClaw](https://github.com/memovai/mimiclaw) | `bb10ea0149080d506d920c09054f4c5b20409de2` | MIT | Uses inbound and outbound queues, a ReAct loop, SPIFFS sessions, cron, Telegram and Brave or Tavily search. The current code allocates large PSRAM buffers, parses complete search responses with cJSON, permits multiple TLS-heavy tasks, and carries optional WebSocket, OTA, AP portal, proxy, Feishu, skills and file features in one build. Its documented task stacks total about 40 KB and several buffers are 16 to 32 KB each. |
| [NanoClaw](https://github.com/qwibitai/nanoclaw) | `0b034342fc19fea2c95da20c2a42b4eaa31f5d84` | MIT | Uses strict channel routing, one-writer message stores, per-agent isolation, scheduled-message gates and small always-loaded memory indexes. Containers, SQLite and credential proxies require Linux and are not portable to ESP32. |
| [PicoClaw](https://github.com/sipeed/picoclaw) | `85dcfccad66dd0dca0ec9765976b665d8ebf7191` | MIT | Separates channels, a bounded message bus, provider adapters, tool policy, approval hooks, JSONL memory and a timer-driven scheduler. Current builds target Linux and have grown beyond the original sub-10 MB claim. Parallel tool execution and broad runtime extension would create avoidable ESP32 memory pressure. |
| [ESP-Claw](https://github.com/espressif/esp-claw) | `51c5020249db65f604a07356771c91232ea50afc` | Apache-2.0 | Confirms ESP-IDF-native agent loops, event routing, structured memory, Telegram and reusable HTTP transport are practical. Its Lua runtime, dynamic capability system, UI and broad board modules exceed MicroPaw's scope. |

No source code is copied from these projects. MicroPaw uses independently written implementations and preserves the licence notices of ESP-IDF only.

## Verified platform and service contracts

- [ESP HTTP Client](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_http_client.html) supports explicit `open`, `fetch_headers`, repeated `read`, `close` and `cleanup`. MicroPaw will use that stream-reader path and a single mutex around outbound HTTPS.
- [ESP-IDF NVS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/nvs_flash.html) is intended for small values and blobs. It is power-loss tolerant. NVS encryption is effective when paired with flash encryption or the supported HMAC scheme.
- [ESP-NETIF SNTP](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_netif_programming.html) provides `esp_netif_sntp_init` and synchronization waiting. Scheduled wall-clock work starts only after time sync.
- [ESP-IDF RAM guidance](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/performance/ram-usage.html) defines task high-water marks in bytes on ESP32-S3. Heap reports will include current free, minimum free and largest free blocks for internal RAM and PSRAM.
- [Telegram Bot API](https://core.telegram.org/bots/api) documents `getUpdates`, update confirmation through `offset = update_id + 1`, long-poll `timeout`, `allowed_updates` and `sendMessage`. MicroPaw will accept messages only from the configured owner chat.
- [OpenAI function calling](https://developers.openai.com/api/docs/guides/function-calling) documents Responses API function tools, strict JSON schemas, `function_call_output`, streaming call events and `parallel_tool_calls: false`. [Streaming responses](https://developers.openai.com/api/docs/guides/streaming-responses) use SSE and typed events. The current cost-sensitive model is [GPT-5.6 Luna](https://developers.openai.com/api/docs/models/gpt-5.6-luna), which supports Responses, streaming and function calling.
- [OpenRouter Responses](https://openrouter.ai/docs/api/reference/responses/overview) documents `POST https://openrouter.ai/api/v1/responses`, bearer authentication, streaming and tool calling. The endpoint is beta and stateless. Its [multi-turn contract](https://openrouter.ai/docs/api/reference/responses/basic-usage#multiple-turn-conversations) rejects `store: true` and `previous_response_id`, so callers must resend bounded conversation items. Its [tool contract](https://openrouter.ai/docs/api/reference/responses/tool-calling) uses `function_call` and `function_call_output` items and typed SSE events.
- DuckDuckGo Lite accepts form searches at `POST https://lite.duckduckgo.com/lite/` with an URL-encoded `q` body. A 2026-07-22 probe returned HTTP 200 and the expected `result-link` and `result-snippet` classes. The former query-string GET returned an HTTP 202 anomaly page. DuckDuckGo does not publish this as a machine API. The markup, access policy or availability can change without notice.
- [MediaWiki REST search](https://www.mediawiki.org/wiki/API:REST_API/Reference#Search) documents `GET /w/rest.php/v1/search/page`, `q`, `limit` and the `pages` result array. English Wikipedia accepts the endpoint over HTTPS without a key.
- [arXiv API](https://info.arxiv.org/help/api/user-manual.html) documents `search_query`, `start`, `max_results` and Atom results. A direct HTTPS probe of `export.arxiv.org` returned HTTP 200 and `application/atom+xml` on 2026-07-21.
- [RSS 2.0](https://www.rssboard.org/rss-specification) defines channel items with title, link and description fields. MicroPaw also accepts Atom entries through the same bounded XML state machine.
- [OpenAlex authentication](https://developers.openalex.org/api-reference/authentication) now requires an API key and uses a freemium daily budget. Its older no-key access no longer matches this project's no-key source rule, so it is not implemented as a provider.
- [Gmail users.messages.send](https://developers.google.com/workspace/gmail/api/reference/rest/v1/users.messages/send) accepts a base64url RFC 2822 message in `raw` at `POST https://gmail.googleapis.com/gmail/v1/users/me/messages/send` with the `gmail.send` scope.
- [Google Calendar events.insert](https://developers.google.com/workspace/calendar/api/v3/reference/events/insert) creates an event at `POST https://www.googleapis.com/calendar/v3/calendars/primary/events`; `start.dateTime` and `end.dateTime` use RFC3339.
- [Google OAuth refresh](https://developers.google.com/identity/protocols/oauth2/web-server#offline) documents an HTTPS form POST to `https://oauth2.googleapis.com/token` with `client_id`, optional `client_secret`, `refresh_token` and `grant_type=refresh_token`. Initial consent remains an external browser step.

## Decisions

- Target the detected ESP32-S3 with 16 MB flash and 8 MB embedded PSRAM using ESP-IDF 6.0.2 and C only.
- Use one fixed inbound queue and one agent task. The scheduler and Telegram poller enqueue compact value messages. No task transfers heap-owned message strings.
- Use an explicit agent state machine: idle, context, inference, tool, response and error.
- Keep tool definitions in a static registry. Disable parallel model tool calls and cap iterations through Kconfig.
- Keep inference configuration separate from the agent loop. Support OpenAI, OpenRouter and a user-supplied Responses-compatible HTTPS endpoint through one stream parser. Store provider, model, endpoint and bearer key in NVS. Use `gpt-5.6-luna` as the configurable OpenAI default.
- Put large, fixed work buffers in PSRAM. Keep task stacks and queue items in internal RAM. Do not construct a full JSON response tree for LLM streams, search results or fetched pages.
- Use one HTTPS transport and one static mutex. No poller, LLM call or tool call can hold a second simultaneous network connection.
- Store compact memory records, recent conversation state and fixed scheduler records as NVS blobs. Avoid a filesystem and repeated Markdown parsing.
- Implement DuckDuckGo Lite as the first provider behind a small interface. Parse its HTML incrementally without a DOM, cJSON or a full-response buffer. Add optional Wikipedia, arXiv and RSS or Atom readers backed by their documented formats. Do not claim DuckDuckGo reliability until repeated connected-board tests pass.
- ESP-IDF 6.0 no longer includes its earlier `json` component. Use one bounded JSON slice parser for LLM events, Telegram, OAuth responses and tool arguments instead of adding cJSON as an external dependency.
- Require owner-chat authorization. Give email and Calendar independent `allowed`, `permission` and `disabled` modes stored in local configuration. Treat fetched web text as untrusted data and never as instructions.
- Use direct Gmail and Calendar REST calls with an externally provisioned refresh token. No relay is needed after consent. OAuth browser consent is outside the ESP32.
- Provision secrets over the local serial console, individually or through a bounded TOML push, into NVS. Never compile or commit credentials. Provide a separate production security config for NVS and flash encryption, but do not enable irreversible eFuse settings during development flashing.
- Make Telegram, search, page fetch, Gmail and Calendar removable through Kconfig. Exclude GPIO, WebSocket, OTA, AP portal, Lua, file tools and unrelated hardware features.
- Generate build size reports from `idf.py size` and `size-components`. Print runtime heap and task watermarks from the board. Compare current zclaw and MimiClaw builds on the same ESP32-S3 toolchain when their pinned IDF versions permit it.

## Measured benchmark

All three projects were built locally on 2026-07-21 for ESP32-S3 with 16 MB flash and 8 MB octal PSRAM. zclaw used its current source plus a benchmark-only hardware defaults overlay. MimiClaw used its current defaults. No comparison source code was changed. Each project used its declared supported ESP-IDF line, so compiler and framework version differences remain.

| Project | Source revision | ESP-IDF | Image bytes | Padded binary | Static DIRAM | External BSS |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| MicroPaw | `98c026e` before TOML push | 6.0.2 | 877,344 | 877,456 | 133,760 | 120,388 |
| zclaw | `e3ad271244c1f2e01f4df6e81c2bab346e95d1b1` | 5.4 | 847,950 | 848,064 | 173,731 | 0 reported |
| MimiClaw | `bb10ea0149080d506d920c09054f4c5b20409de2` | 5.5.2 | 1,180,553 | 1,180,672 | 135,783 | 0 reported |

MicroPaw is 29,394 image bytes larger than the comparable zclaw build and uses 39,971 fewer static DIRAM bytes. It includes direct search, bounded page fetch, Wikipedia, arXiv and RSS or Atom support that zclaw lacks. MicroPaw is 303,209 image bytes smaller than MimiClaw and uses 2,023 fewer static DIRAM bytes. Its 120,388 bytes of fixed external BSS hold bounded request, stream, tool and channel work areas. Competitor firmware was not flashed, so no competitor runtime heap or stack numbers are presented as measured.

zclaw separately publishes 853,034 image bytes and about 149 KB DRAM used for its default classic ESP32 build. That repository figure is not mixed into the ESP32-S3 table.
