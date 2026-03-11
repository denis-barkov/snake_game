# Changelog

## 2.8.17 - 2026-03-11
- Switched production TLS architecture to ACM + ALB termination: Terraform now looks up an existing tagged `ISSUED` ACM certificate and attaches it to an HTTPS listener.
- Added dual-subnet public VPC layout and ALB target-group wiring to ASG instances; Route53 now aliases the game domain to the ALB.
- Tightened EC2 ingress so app traffic is accepted from ALB security group on HTTP only (public 443 no longer terminates on instance).
- Added explicit ACM lifecycle tooling: `make ssl-cert-create` and `make ssl-cert-delete` with fixed `project=snake-game` tagging and env/domain tags.
- Updated runtime proxy configs to HTTP listener mode behind ALB, eliminating per-rebuild certificate issuance on EC2.
- Updated documentation for the new manual certificate workflow and Terraform certificate discovery behavior.

## 2.8.16 - 2026-03-11
- Removed remaining password-auth artifacts from core storage/domain flow (no username/password fields or username lookup path in server storage logic).
- Removed runtime seed modes and smartseed/admin seed commands; `snake_server` now supports only `serve|reset` and `snakecli app` supports only `reset|reload`.
- Added static startup seed system via `tools/apply_seed_config.py` with `SEED_ENABLED`, `SEED_CONFIG_PATH`, and `APP_ENV` gating; seeding runs only on empty user tables.
- Added seed config example at `seeds/dev.seed.yaml` with documented economic consistency notes and globally unique snake-name expectations.
- Updated local/prod boot/deploy wiring to run the static seed applier before server start, and removed legacy seed systemd units.
- Updated infra/local table definitions to remove legacy username GSI from the users table.

## 2.8.15 - 2026-03-10
- Removed password login flow from backend and frontend; auth is now Google-only across local and prod (`POST /auth/login` now returns `410 password_auth_removed`).
- Updated local defaults for parity with prod by enabling Google auth flags in `Makefile` and removing implicit local seeding from `make local-setup`.
- Added explicit seeded local setup target (`make local-setup-seeded`) and updated smoke tooling to use an existing bearer token instead of password credentials.

## 2.8.14 - 2026-03-09
- Fixed global snake-name uniqueness regression that allowed duplicate names under buffered persistence timing.
- Added runtime+durable global snake-name existence checks for onboarding/create/rename validation paths.
- Fixed buffered SQLite snake snapshot schema/flush path to persist `snake_name` and `snake_name_normalized` fields, including backward-compatible column migration.

## 2.8.13 - 2026-03-09
- Added a shared snake action resolver used by `attach`, `rename`, and `delete` to remove PROD/local lookup drift and stop false `snake_not_found` on visible owned snakes.
- Added structured action lookup diagnostics for snake actions, including panel-visible IDs vs action-lookup IDs and lookup layer/profile details.
- Replaced `Create Snake` browser `prompt()` with an in-app modal matching rename/delete modal behavior.

## 2.8.12 - 2026-03-09
- Fixed onboarding starter snake race in prod where immediate durable read could fail and return `starter_snake_visibility_failed` despite successful runtime creation.
- Switched onboarding starter verification to runtime owned-snake visibility first, with durable fallback only for legacy-existing starter records.
- Added rollback/logging guards so failed starter verification does not leave silent partial onboarding state.

## 2.8.11 - 2026-03-08
- Fixed snake creation atomicity: `/me/snakes` now validates visibility/name via owned-snake view and rolls back on failure so no orphan snake remains after an error.
- Enforced no-unnamed-snake creation invariant in world/service create path and user-facing list serialization.
- Added snake deletion endpoint (`POST /snake/{id}/delete`) with ownership checks, liquid-asset refund, and name release for future reuse.
- Replaced browser-native rename/delete snake dialogs with app-styled custom modals in the My Snakes menu.

## 2.8.10 - 2026-03-08
- Fixed snake creation naming persistence: onboarding and manual create now persist the user-provided snake name in the initial create snapshot (no create-then-rename path).
- Added strict create-time verification so create fails if the newly created snake is not stored with the requested validated name.
- Updated runtime snake snapshots to carry `snake_name` and `snake_name_normalized`, improving immediate `/me/snakes` name visibility without numeric fallback labels.

## 2.8.9 - 2026-03-08
- Enforced global snake-name uniqueness across onboarding, create, and rename flows using `snake_name_normalized` checks end-to-end.
- Added a DynamoDB snake-name lookup index (`gsi_snake_name_normalized`) with backward-compatible scan fallback for older tables.
- Hardened name persistence conflict handling so API returns `409 {"error":"snake_name_taken"}` consistently (including post-create rollback path).
- Updated frontend create/rename/onboarding handling to show a clear retry message when a snake name is already taken.

## 2.8.8 - 2026-03-08
- Fixed attach regression by removing create-on-miss fallback: `/snake/{id}/attach` now fails cleanly without creating new snakes.
- Switched `GET /me/snakes` to a durable-first read path for snake id/name consistency across runtime and buffered persistence layers.
- Added named snake creation (`POST /me/snakes` with `snake_name`) and snake rename endpoint (`POST /snake/{id}/rename`).
- Updated player sidebar identity UX to show Company Name and render stored snake names instead of numeric fallback labels.
- Added runtime config `AUTO_SEED_ON_START` (default `false`) to keep local startup behavior clean and production-like by default.

## 2.8.7 - 2026-03-08
- Fixed borrow treasury guard expression to use valid DynamoDB condition syntax (`m_gov_reserve >= :a`) and avoid validation-write failures.
- Kept borrow debit/credit rollback semantics while improving local DynamoDB compatibility.
- Removed a failing `if_not_exists(...)` condition usage from borrow treasury debit path.

## 2.8.6 - 2026-03-08
- Reworked borrow persistence to a deterministic treasury-debit/user-credit path without Dynamo transaction dependency.
- Replaced generic borrow rejection branch with explicit `persistence_write_failed`/`unauthorized` mappings in the storage path.
- Improved local/prod parity for borrow by using the same guarded non-transaction update semantics.

## 2.8.5 - 2026-03-08
- Added signed-in auth UI behavior that hides Google sign-in and shows a `Log out` action with full local session cleanup.
- Hardened client attach flow to refresh/repair selected snake id before calling `/snake/{id}/attach`.
- Added structured production diagnostics for borrow/attach actions with profile, policy result, and rejection reason fields.
- Added a minimal API smoke test (`tools/smoke_economy_flow.py`) and Make target (`smoke-economy-local`) for pre-release critical-flow checks.

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
