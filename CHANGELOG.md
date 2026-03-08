# Changelog

## 2.8.4 - 2026-03-08
- Hardened attach endpoint fallback resolution by checking DB-owned snakes and auto-creating a starter snake when none exists, preventing false `snake_not_found` failures after rebuilds.
- Added explicit borrow pre-validation for missing authenticated user rows so borrow failures return `user_not_found` instead of generic policy errors.
- Improved attach reliability when runtime snake cache lags behind persistence by resolving ownership from durable snake records first.

## 2.8.3 - 2026-03-08
- Fixed borrow flow reliability when `TransactWriteItems` is unavailable by adding a guarded fallback path with treasury rollback on partial failure.
- Improved borrow rejection accuracy so treasury guard failures return `insufficient_treasury` instead of generic policy errors.
- Hardened attach endpoint resolution to fall back to the authenticated user's starter/only snake when stale snake IDs are sent by clients.
- Updated AWS IAM DynamoDB policy to include `dynamodb:TransactWriteItems` for clean rebuild compatibility.

## 2.8.2 - 2026-03-08
- Fixed borrow transfer accounting to debit treasury and credit user liquidity atomically.
- Added explicit borrow rejection reasons (`invalid_amount`, `insufficient_treasury`, `policy_rejected`, `internal_error`).
- Fixed treasury-row compatibility for legacy `economy_params` keys (`active` and `global`) in borrow flow.
- Hardened labor counting to only count successful active snake advances.
- Added backend validity flags for `price_index` and `inflation` during zero-output periods.

## 2.8.1 - 2026-03-08
- Fixed borrow accounting so treasury is debited atomically when user liquid assets are credited.
- Added treasury-liquidity rejection for borrow when reserve is insufficient.
- Tightened labor counting to successful active snake advances only.
- Raised auto-expansion target spatial ratio default from 2.6 to 3.2 for less crowded expansions.
- Added global economy visibility for field size, free space, system reserve, spatial ratio, and stabilization status.
- Improved client handling after expansion by forcing live mask/cache refresh on expansion events.

## 2.8.0 - 2026-03-08
- Added an automatic spatial and monetary stabilization engine with derived reserve accounting.
- Added scheduled fast spatial checks tied to configurable economic period duration.
- Added liquidity-constraint mode transitions with deduplicated global system notifications.
- Added period-close monetary stabilization with capped treasury-only automatic expansion.
- Added stabilization telemetry to admin economy status and `snakecli economy status`.

## 2.7.0 - 2026-03-07
- Added Google Sign-In as the live player entry path.
- Added first-time onboarding with company naming, starter snake naming, and starter liquid assets.
- Added account deletion flow in Settings with exact company-name confirmation.
- Improved returning-user login transition with short world tips before entering play.

## 2.6.0 - 2026-03-07
- Added the World Evolution Log in-game panel with a full release archive.
- Added a top-right version badge so players can quickly check update history.
- Added a one-time "The World Has Evolved" announcement for new releases.
- Improved spectator startup camera behavior for more consistent first view.

## 2.5.0 - 2026-03-06
- Improved world streaming stability for larger maps.
- Reduced visual flicker around chunk boundaries during movement.
- Improved snake follow camera consistency across zoom levels.
- Updated in-game economy panel reliability under reconnects.

## 2.4.0 - 2026-03-05
- Added map-style zoom behavior with better viewport consistency.
- Improved watch mode controls in the snake list UI.
- Improved mobile and tablet rendering stability in the game field.
- Added safer defaults for spectator camera and controls.

## 2.3.0 - 2026-03-04
- Added richer economy panels with personal and global indicators.
- Improved tooltips to better explain economy values to players.
- Added period countdown visibility for easier session planning.
- Improved dashboard responsiveness on smaller screens.

## 2.2.0 - 2026-03-03
- Improved login/session continuity after page refresh.
- Improved attach and borrow action feedback in the user panel.
- Added stronger validation for player economy actions.
- Improved reliability of user snake list updates.

## 2.1.0 - 2026-03-02
- Added improved world camera behavior for spectators.
- Improved WebSocket reconnect behavior during temporary network drops.
- Improved visibility of live game status values.
- Improved controls for selecting and tracking player snakes.

## 2.0.0 - 2026-03-01
- Added chunk-ready world replication architecture for scalability.
- Improved world rendering performance for larger active sessions.
- Added stronger routing for public and private game streams.
- Improved overall game stability for long-running sessions.

## 1.9.0 - 2026-02-28
- Improved snake collision outcomes to make fights more predictable.
- Improved direction handling to avoid confusing movement edge cases.
- Improved event handling around snake elimination and recovery.
- Improved consistency of snake state updates after conflicts.

## 1.8.0 - 2026-02-27
- Added economy read-only HUD indicators for live sessions.
- Improved economy data refresh consistency for connected players.
- Added better fallback behavior when economy data is temporarily unavailable.
- Improved dashboard clarity for key world health values.

## 1.7.0 - 2026-02-26
- Improved local and cloud parity for test and deployment flows.
- Improved startup and shutdown behavior for local runtime sessions.
- Added stronger reliability checks for server and proxy startup.
- Improved production rollout consistency across clean environment rebuilds.
