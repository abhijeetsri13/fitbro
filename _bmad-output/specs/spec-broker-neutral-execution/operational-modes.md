# Trading Modes, Kill Switches & Market Calendar

Backs **CAP-17, CAP-18, CAP-21**. Operational controls that gate what the library is allowed to do, and when.

## Trading modes (CAP-18)

Selected by configuration. Modes exist to develop and operate the system safely.

| Mode | Behavior |
|---|---|
| Live | Sends real orders. |
| Paper | Simulates orders without broker execution. |
| Dry-run | Validates orders but does not execute. |
| Replay | Runs strategy on recorded market data. |
| Monitor-only | Tracks positions and orders without placing new ones. |
| Exit-only | Blocks entries, allows exits. |
| Emergency | Allows only cancel and square-off behavior. |

## Kill switches (CAP-17)

Multiple, independent kill switches.

| Kill-switch type | Behavior |
|---|---|
| Soft kill | Block new entries, allow exits. |
| Strategy kill | Stop one strategy only. |
| Broker kill | Stop using one broker only. |
| Account kill | Stop all trading for one account. |
| Panic kill | Cancel open orders, square off positions, block future entries. |

**Triggers (manual or automatic):** manual command; daily loss breach; unknown order; position mismatch; broker instability; stale market data; repeated order rejection; strategy bug.

## Market calendar & time awareness (CAP-21)

Understands Indian market timings. Supports:

- Market open and close
- Pre-open session
- Normal session
- Expiry day
- Holidays
- Special trading sessions
- Muhurat trading
- Strategy-specific allowed time windows
- Entry cut-off time
- Square-off time

**Required behavior:**
- No new intraday entries after the configured cut-off time.
- No order placement when the exchange is closed unless AMO is explicitly configured.

**Calendar data source (CAP-21):** the holiday / special-session / Muhurat calendar is *reference data*, sourced from a configured provider (broker feed, exchange-published list, or bundled file), refreshed, cached across restarts, and **staleness-gated like the instrument master** — a stale or missing calendar blocks trading at safe-start (CAP-28) rather than risking orders on a holiday or a missed special session. New holidays / special sessions take effect only after a refresh.

## Interactions

- **Exit-only mode** and **soft kill** overlap in intent (block entries, allow exits) but differ in scope/origin: exit-only is a configured run mode; soft kill is a triggered control. Both must independently block entries.
- **Emergency mode** is the run-mode equivalent of the posture a **panic kill** leaves behind: only cancel/square-off permitted.
- Kill-switch and mode state are part of the validation gate (`risk-controls.md`): the kill-switch check is the last gate before dispatch.
