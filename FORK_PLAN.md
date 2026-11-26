# Moltorino Fork Plan: Watch-Time, Channel Points & Twitch Drops

This plan outlines how to extend **Moltorino** (C++/Qt) with the watch-time, bonus-chest claiming, and Twitch Drops capabilities found in **StreamNook**, while keeping Moltorino's native Qt chat UI and avoiding StreamNook's multi-platform/extra bloat.

---

## Current Baseline in Moltorino

Moltorino already has most of the foundational plumbing needed:
- **Twitch GQL & Auth:** `src/providers/twitch/api/TwitchGql.cpp` (including `TWITCH_GQL_BROWSER_CLIENT_ID` and `TWITCH_GQL_TV_CLIENT_ID` / `MoltorinoAuth`).
- **PubSub:** `src/providers/twitch/PubSubClient.cpp` already subscribes to `community-points-user-v1.<userID>` and parses `claim-available` (`PubSubCommunityPointsUserV1Message::Type::ClaimAvailable`), forwarding it to `userPoints.updated`.
  - Currently, `TwitchChannel::handleUserPointsUpdate` (`src/providers/twitch/TwitchChannel.cpp`) returns early for anything other than `points-earned` and `points-spent`.
- **Channel Points UI:**
  - Balance display in `src/widgets/splits/SplitInput.cpp`.
  - Reward redemption dialog in `src/widgets/dialogs/ChannelPointsDialog.cpp` (`/redeem`).
  - Predictions in `src/widgets/dialogs/PredictionDialog.cpp` (`/prediction`, `TwitchGql::makePrediction`).
  - Channel-point poll voting in `src/widgets/dialogs/PollDialog.cpp`.
- **Stream Launching:** `src/util/StreamLink.cpp` (`openStreamlinkForChannel`), which spawns `streamlink.exe` externally without any Twitch watch-time or Drops reporting.

---

## Architecture Overview

To match StreamNook's crediting behavior in Moltorino, four components are needed:

1. **Bonus Chest Claiming (`ClaimCommunityPoints`)**
2. **Watch-Time Heartbeat (`spade.twitch.tv/track` + `sendSpadeEvents`)**
3. **Player-Shaped HLS Segment Traffic (for Drops)**
4. **Twitch Drops Service & Qt UI**

---

## Phase 1: Bonus Chest Claiming (Quickest Win)

**Goal:** Detect and claim periodic channel-point bonus chests (manually via a button in chat input, and/or automatically).

1. **GQL Mutation (`src/providers/twitch/api/TwitchGql.hpp/.cpp`):**
   - Add `claimChannelPoints(channelId, claimId, token, ...)`:
     - Mutation: `ClaimCommunityPoints($input: ClaimCommunityPointsInput!) { claimCommunityPoints(input: $input) { claim { pointsEarnedTotal } currentPoints } }`
     - Mirrors StreamNook's `DropsService::claim_channel_points` (`src-tauri/src/services/drops_service.rs`).
2. **Handle `claim-available` in `TwitchChannel` (`src/providers/twitch/TwitchChannel.hpp/.cpp`):**
   - Update `TwitchChannel::handleUserPointsUpdate` to handle `type == "claim-available"`.
   - Extract `claim.id` and `claim.channel_id` from the PubSub payload and store an `availableClaimId_` per channel (with deduplication against in-flight/already-claimed IDs, similar to StreamNook's `watched_chest.rs`).
   - Add a `availableClaim` fallback read inside `ChannelPointsContext` (`TwitchGql::getChannelPoints`) so chests aren't missed if PubSub reconnects.
3. **UI & Settings (`SplitInput.cpp`, `Settings.hpp`):**
   - Add a setting (e.g. `autoClaimChannelPoints`) to auto-claim chests for the active/watched channel (or any open live channel).
   - In `SplitInput`, show a chest indicator/amber state on `channelPointsLabel` when a claim is available (and optionally a "+N" toast/indicator on gain). Clicking it triggers `claimChannelPoints`.

---

## Phase 2: Watch-Time Heartbeat Service

**Goal:** Credit passive watch-time channel points (and half of the Drops requirement) for the channel the user is watching.

> **Reference:** StreamNook's `src-tauri/src/services/watch_heartbeat_service.rs`.

1. **New Singleton/Service (`src/providers/twitch/WatchHeartbeatService.hpp/.cpp`):**
   - Tracks a single active `WatchTarget` (`channelId`, `login`, `broadcastId`, `gameId`, `gameName`) so only the actively watched channel is reported.
   - Runs a 60-second `QTimer` while watching is active.
2. **Two Transports per Minute:**
   - **Channel Points + Primary Drops:** Form-POST `data=<base64(json)>` to `https://spade.twitch.tv/track` with `"event": "minute-watched"` and properties (`broadcast_id`, `channel_id`, `channel`, `hidden: false`, `live: true`, `location: "channel"`, `logged_in: true`, `muted: false`, `player: "site"`, `user_id`). HTTP `204` = accepted.
   - **Backup Drops:** `sendSpadeEvents` GQL mutation (`gzip` + `base64` encoded `minute-watched` payload with `repository: "twilight"`, `encoding: "GZIP_B64"`).
3. **How "Watching" is Triggered:**
   - **Option A (Streamlink-coupled):** Track the `QProcess` launched by `StreamLink.cpp` (`openStreamlinkForChannel`); while that process is alive for a live Twitch channel, set that channel as the `WatchTarget`.
   - **Option B (Explicit "Watch / Farm" toggle per split):** A split header button or `/watch` command that starts/stops the watch session for that channel without needing an external player.
   - **Option C (Embedded player):** Embed `libmpv` or `QWebEngineView` in a collapsible split pane and tie `WatchTarget` to playback state.

---

## Phase 3: Drops Playback Traffic (Required for Twitch Drops)

**Goal:** Satisfy Twitch's requirement that Drops progress only counts when media segments are requested with a viewer-signed playback token.

> **Reference:** StreamNook's `src-tauri/src/services/drop_playback.rs`.

1. **New Service (`src/providers/twitch/DropPlaybackService.hpp/.cpp`):**
   - Follows the active `WatchTarget` from `WatchHeartbeatService` when Drops tracking is enabled.
   - Calls GQL `PlaybackAccessToken` (`streamPlaybackAccessToken(channelName: $login, params: { platform: "web", playerBackend: "mediaplayer", playerType: "site" })`) using the user's OAuth token.
   - Fetches the master HLS playlist from Usher (`usher.ttvnw.net`), picks the **lowest-bandwidth** video rendition, and polls its media playlist.
   - Sends an HTTP `HEAD` request for each new segment URI (deduplicated in a bounded ring buffer of ~256 URIs), downloading **no audio or video**.
   - Re-resolves the playback token every ~10 minutes before expiry, with exponential backoff on offline/error responses.

---

## Phase 4: Twitch Drops Service & UI

**Goal:** Discover active Drop campaigns, track progress, auto-claim completed drops, and display status inside Moltorino.

> **Reference:** StreamNook's `src-tauri/src/services/drops_service.rs`.

1. **GQL Operations (`TwitchGql` / `DropsService`):**
   - `Inventory`: Fetch in-progress campaigns, claimed drops, and per-drop `currentMinutesWatched` / `requiredMinutesWatched` / `dropInstanceID`.
   - `ViewerDropsDashboard` + `DropCampaignDetails`: Fetch active campaigns, eligible channels/games, and account-link status.
   - `DropsPage_ClaimDropRewards`: Claim a finished drop using its `dropInstanceID`.
2. **Real-Time PubSub Updates (`PubSubClient.cpp`):**
   - Subscribe to `user-drop-events.<userID>` to receive `drop-progress` and `drop-claim` notifications in real time without relying solely on polling.
3. **Qt UI & Chat Integration:**
   - **Drops Dialog (`src/widgets/dialogs/DropsDialog.hpp/.cpp`):** Lists active campaigns, current progress bars, connected-account status, and claimed/ready-to-claim drops.
   - **Chat / Split Integration:**
     - Optional status pill or toolbar icon showing active drop progress (e.g., `45/60m`) for the current channel's category.
     - System chat messages / desktop toasts (`Toasts.cpp`) when a drop is ready or auto-claimed.
     - Optional `/drops` command in `CommandController.cpp` to open the Drops dialog.

---

## Suggested Implementation Order

1. **Phase 1 (Bonus Chests)** — Smallest scope; immediately tests `ClaimCommunityPoints` and `claim-available` PubSub handling.
2. **Phase 2 (Watch Heartbeat)** — Unlocks passive watch-time channel points and lays the session foundation.
3. **Phase 3 (HLS `HEAD` Segment Traffic)** — Small, self-contained network loop required before Drops progress will move.
4. **Phase 4 (Drops Service + Qt UI)** — Campaign/inventory GQL, `user-drop-events` PubSub, auto-claim, and the Qt Drops dialog.
