# LuckyMoltorino7 Improvements Plan

Plan for MPV Streamlink integration, automatic watch-time/Drops tracking, Channel Points popup controls, and "Wait for Stream" auto-launch.

---

## 1. Use `mpv` Instead of VLC with Streamlink

- **Settings (`src/singletons/Settings.hpp`, `src/widgets/settingspages/ExternalToolsPage.cpp`):**
  - Add `BoolSetting streamlinkUseMpv{"/external/streamlink/useMpv", true};`
  - Add `QStringSetting streamlinkMpvPath{"/external/streamlink/mpvPath", ""};`
  - Add a `"Use mpv as Streamlink player"` checkbox and optional `"Custom mpv path"` input under the **Streamlink** section in `ExternalToolsPage.cpp`.
- **Streamlink Launch (`src/util/StreamLink.cpp`):**
  - In `openStreamlink()`, when `streamlinkUseMpv` is enabled and `streamlinkOpts` does not already specify `--player`, pass `--player <mpv-path>` to Streamlink.
  - Auto-detect `mpv.exe` / `mpv` on `PATH` or at `C:\Program Files\MPV Player\mpv.exe` on Windows when `streamlinkMpvPath` is empty.

---

## 2. Auto-Enable "Watch for Points & Drops" When Opening a Live Stream

- **Settings (`src/singletons/Settings.hpp`, `src/widgets/settingspages/MoltorinoPage.cpp`):**
  - Add `BoolSetting autoWatchActiveChannel{"/moltorino/watch/autoWatchActiveChannel", true};`
  - Add `"Automatically watch active live Twitch channel for Points & Drops"` under **Watch time and Twitch Drops**.
- **Service & Split Hooks (`src/providers/twitch/WatchHeartbeatService.hpp/.cpp`, `src/widgets/splits/Split.cpp`):**
  - Add `WatchHeartbeatService::setAutoWatchChannel(const QString &login)` with priority handling so an active Streamlink session or an explicit `/watch off` override is respected.
  - Connect to `Split::setChannel`, `Split::focused`, and `TwitchChannel::streamStatusChanged`: when the active/focused Twitch split is live and `autoWatchActiveChannel` is enabled, automatically start `WatchHeartbeatService` (which also triggers `DropPlaybackService` and `DropsService` inventory polling) without requiring the 3-dots menu.
  - Clear auto-watch when the channel goes offline or the split closes (unless a manual or Streamlink watch session is active).

---

## 3. Channel Points Popup Controls (`ChannelPointsDialog`)

Add a compact control bar directly below the header in `src/widgets/dialogs/ChannelPointsDialog.cpp` (the rewards popup opened by clicking the points balance):

- **Control Bar Buttons** (styled consistently with `ChannelPointsUtilityButton` + checked highlight):
  1. **`Watch: ON / OFF`** — Calls `WatchHeartbeatService::toggleWatching(channel_->getName())` and stays in sync via `WatchHeartbeatService::targetChanged`.
  2. **`Auto-redeem: ON / OFF`** — Toggles `getSettings()->autoClaimChannelPoints` (auto-claiming periodic bonus chests) and updates live via setting change signals.
  3. **`Drops`** — Opens `DropsDialog::showDialog(this)`.
- **Bonus Chest Button (conditional):**
  - When `channel_->hasAvailableBonusClaim()` is true (e.g. while `Auto-redeem` is off), show a `"Claim Bonus Chest"` button in the bar to claim it directly from the popup.

---

## 4. "Wait for Stream" Mode (Auto-Open `mpv` When Stream Goes Live)

Allow waiting on an offline Twitch channel with **both** modes supported (configurable in Settings and/or directly in the Split Header / Channel Points popup):

- **Entry Points:**
  - Split header 3-dots menu (`src/widgets/splits/SplitHeader.cpp`): `"Wait for stream (open idle mpv)"`, `"Wait for stream (background)"`, or `"Cancel waiting"`.
  - `ChannelPointsDialog` control bar and `/watchwait [idle|bg|off]` command (`src/controllers/commands/builtin/twitch/WatchAndDrops.cpp`).
- **Mode A — Idle `mpv` Window Immediately (Recommended for positioning on another monitor):**
  - Launches `mpv` right away with `--idle=yes --force-window=yes --title="Waiting for <channel>..."` and an IPC pipe (`--input-ipc-server=\\.\pipe\luckymoltorino-mpv-<channel>` on Windows, `/tmp/luckymoltorino-mpv-<channel>.sock` on Unix).
  - The user can move/resize the `mpv` window to any monitor while waiting.
  - Moltorino monitors that `mpv` PID (if the user closes the window, waiting is cancelled) and listens for `TwitchChannel::streamStatusChanged` (plus a ~30s `Helix::fetchStreams` fallback).
  - The moment the stream goes live, Moltorino runs `streamlink --stream-url twitch.tv/<channel> <quality>` (or falls back to Streamlink's local HTTP stream) and sends `{ "command": ["loadfile", "<stream-url>", "replace"] }` over the `mpv` IPC pipe so **that exact `mpv` window** grabs and plays the stream immediately, while `WatchHeartbeatService` tracks the `mpv` PID for Points & Drops.
- **Mode B — Silent Background Wait:**
  - Waits in Moltorino without opening a player window until `live` flips to `true`, then launches `openStreamlinkForChannel(login)` normally.
