# Commands

`/help` lists chat commands.

`/metrics` reports request, inference, tool, context, delivery, media, transcription, missed-reminder and briefing counters without message content.

`/memory` lists saved facts, and `/jobs` lists scheduled jobs.

`/missed` lists pending and delivered missed reminders. `/missed clear` removes delivered history and keeps pending deliveries.

`/briefing` shows the current briefing setting. `/briefing on`, `/briefing off` and `/briefing HH:MM` change it immediately for the owner.

`/forget` clears conversation context while keeping saved facts and jobs.

`/reset` starts the owner-only two-step reset, and `/reset cancel` cancels it.

`/update` installs the latest signed release for this project and skips the current version.

`/confirm ID` approves a pending permission, and `/cancel ID` rejects it.

## Serial console

The serial console supports `config`, `set KEY VALUE`, `push-config BYTES`, `metrics`, `submit TEXT`, `reset-state YES`, `erase-config YES` and `reboot`.
