# K4-Guilds → in-game runtime for the site-owned `clans_*` database

Status: design v2 (2026-10-18), native migration v5 (2026-11-01). v1 (a full K4 port with in-game
create/join/roles/shop) is superseded: the CS2 clan system is owned by the website module
`.scratch/ref-k4/clans-site/module_page_clans2`, and the Plugify
plugin is only the **in-game runtime**.

Sources:

* Site: `.scratch/ref-k4/clans-site/module_page_clans2/` (`forward/data.php`, `ext/Clans.php`,
  `views/**`, `translation.json`, `description.json`).
* Live DB schema: `.scratch/ref-k4/clans-db/schema.sql`.
* Old Metamod clan config: `.scratch/ref-k4/clans-config/*.ini`.
* Port infrastructure: `libs/plgdb`, `docs/PLUGIN_STANDARD.md`,
  `handoff/weaponskins/PITFALLS.md`, `plugins/k4_missions` (the sibling port's layout).

---

## 1. Decisions v2 (site-owned)

The site implements, and the plugin must not duplicate:

| Flow | Site evidence |
|---|---|
| Clan creation (name/desc/tag/type, roles, owner member, upgrade row, log) | `data.php:128–309` (INSERTs `:245–286`, log `:304`) |
| Clan delete (cascade incl. logs) | `data.php:320–341`, `Clans.php:1091–1107` |
| Join / exit | `data.php:808–864` (open/request), `:343–367` (exit) |
| Kick | `data.php:369–397` |
| Role assignment | `data.php:399–436` |
| Role add / edit / delete | `data.php:478–593` |
| Leadership transfer | `data.php:438–476` |
| Join requests accept/cancel (single + all) | `data.php:595–703` |
| Settings: name, tag, `desc`, **`type`** | `data.php:705–776` (`UPDATE clans_main … type` `:746–747`) |
| Reactions / views | `data.php:778–806`, `Clans.php:753–794` |
| Services: buy slots/roles/rating/bank/duration | `data.php:866–996` (buy `:929–996`) |
| Levels (`levels.json`, exp thresholds, admin CRUD) | `Clans.php:879–907`, `data.php:1090–1149` |
| Logs (write + admin view) | `Clans.php:1057–1065`, `data.php:1151–1172` |

Consequences:

1. **`clans_main.type` is website-only** (open/request/invite handling is the site's business).
   The plugin never uses it for logic; it may display it.
2. **Roles are created and assigned only on the site.** `clans_roles.type`: 1 = owner role,
   2 = participant/default role, 3 = custom role (`Clans.php:317–333`; creation
   `data.php:266–280`; accepted requests get type 2 `:628–633`). The site uses only the
   `kick`, `roles`, `invite`, `settings` flags (`Clans.php:801–823`, `data.php:372,405,483,538,
   575,600,644,657,696,709`); `services` is owner-only and there is no separate column for it.
3. **Slots are bought and granted only on the site.** The site stores purchases in
   `clans_upgrade.slots_up` / `roles_up` (`data.php:979–984`) and computes the effective caps as
   `clans_main.slots + slots_up * serviceStep('slots')` and
   `role_count + roles_up * serviceStep('roles')` (`Clans.php:361–377`). `serviceStep()` reads
   `items.json` on the web host (not in the DB); the module comment records slots = 10 and
   roles = 3 (`Clans.php:349–359`). The plugin never writes these columns.
4. **`clans_main.duration` is website-only** (purchase period; set on creation `data.php:249`,
   extended by the duration service `:989–992`). The plugin never reads or writes it.
5. **Bank can never go below 0.** The site checks then subtracts (`data.php:967–970`) and can
   add through the bank service (`:987–988`). The plugin uses guarded SQL with clamp (see §4).
6. **`clans_logs`**: the site writes `INSERT INTO clans_logs (clan_id, action, value, created,
   status) VALUES (?, ?, json_encode(...), time(), 1)` (`Clans.php:1057–1065`). The admin log
   view prints `action` and the raw JSON `value` (`data.php:1151–1172`); the clan-page logs
   component (`views/components/logs.php`) is ionCube-encoded and cannot be inspected. The
   plugin writes **no** logs (see §6).
7. **`clans_main.exp` is website-only**: rating service (`data.php:985–986`), transfer penalty
   (`:459–463`), admin levels. The plugin never writes it; it only reads it for display/tops.
8. **`clans_players.kills/deaths/name/lastactivity` are the columns the site displays**:
   member list ordered by `p.kills DESC` (`Clans.php:656–661`), `lastactivity` rendered as
   "time ago" (`Clans.php:930–941`), member/name shown from `clans_players.name`. The plugin
   maintains exactly these four (plus `clans_main.bank`).

---

## 1b. Decisions v3 (in-game management menu)

The old Metamod clan plugin had an in-game management menu; v3 adds it back while the site keeps
everything it owns. Menu implementation: the **native s2sdk menu API** (one menu per display, a
deferred action queue drained from `OnPluginUpdate`, `MenuId 0` reserved by a dummy menu). The
optional `hudmenu` plugin is used only for the player's own `!menu` choice
(`GetClientMenuType`), resolved with the exact `pisexcompat::ResolveS2sdkMenuTypeName` table
(`k4::ResolveMenuTypeName`): `hud`/`wasd` are used only while the matching backend is registered,
otherwise the builtin `button`; the module's `HUD_LAYOUT` default is `hud` when registered,
otherwise `button`. Without hudmenu the builtin types still work. Bare `!clan`, `/clan`, `.clan`,
`!clans` and the console `clans` / `clan_menu` open the menu; the read-only text subcommands stay.

### Menu tree

```
[no clan]
  Clan list          name [tag], type, members/effective slots
    Clan info        name/tag, type, exp+level, bank, slots, owner, created
      Join           type 1 (open)        -> INSERT clans_players
      Apply          type 3 (requests)    -> INSERT clans_request
      Cancel request type 3 + pending     -> DELETE clans_request
      Invite only    type 2               -> disabled line
  Top clans          exp / bank / kills / deaths (config-gated)
  Pending invite     Accept / Decline
[in clan]
  Clan info          same + your role
  Members            member card: role, kills, deaths, joined, last seen
  Top clans
  Management         only the actions the player has rights for:
    Invite           online players not in any clan -> confirm -> target's accept/decline menu
    Kick             members except owner/self -> confirm
    Requests         clans_request list -> Accept / Decline
    Disband          owner only, double confirmation
  Leave              confirm; owner sees a disabled line
```

### Rights (exactly `Clans::getClanAccessCheck`, `Clans.php:801–823`)

| Right | Owner | Member |
|---|---|---|
| Invite | yes | `clans_roles.invite` |
| Requests (accept/decline) | yes | `clans_roles.invite` (the site uses `invite` for requests, `data.php:600,644,657,696`) |
| Kick | yes | `clans_roles.kick` |
| Settings | yes | `clans_roles.settings` (site-only action) |
| Roles | yes | `clans_roles.roles` (site-only action) |
| Disband | yes | **no** (`ClanDelete` checks the owner, `data.php:333–337`) |
| Services | yes | **no** (`Clans.php:819–820`) |

A member with `role_id NULL` or a foreign role has no rights (0 flags). Rights are re-checked on a
freshly loaded guild immediately before every write; the SQL adds its own guards for the target,
owner, type and slots.

### Writes per action (site SQL + race-safe guards)

| Action | SQL / side effects | Guard |
|---|---|---|
| Join (open) | `INSERT INTO clans_players (auth, name, clan_id, joined, lastactivity)` (`data.php:849–850`) | not a member of any clan; `clans_main.type=1`; `COUNT(*) < slots + slots_up*step` |
| Accept invite | same INSERT (no type guard) | invite valid; not a member; slots |
| Apply | `INSERT INTO clans_request (clan_id, steamid, name, date)` (`data.php:857–858`) | `type=3`; not a member; no duplicate request |
| Cancel request | `DELETE FROM clans_request WHERE clan_id=? AND steamid=?` (`data.php:648`) | affected row |
| Leave | `DELETE FROM clans_players WHERE clan_id=? AND auth=?` (`data.php:363`) | `auth <> owner` |
| Kick | `DELETE FROM clans_players WHERE clan_id=? AND auth=?` (`data.php:394`) | `auth <> owner`, `auth <> actor` |
| Accept request | `INSERT INTO clans_players (…, role_id = type-2 role)` from `clans_request` (`data.php:628–633`), then `DELETE FROM clans_request WHERE id=?` (`:634`) | request exists, not a member, slots |
| Decline request | `DELETE FROM clans_request WHERE clan_id=? AND steamid=?` (`data.php:648`) | affected row |
| Disband | MySQL: one atomic multi-table `DELETE p, r, q, rea, v, u, l, m FROM clans_main … LEFT JOIN …` over players, roles, request, reactions, views, upgrade, logs, main — the site's order (`Clans.php:1091–1107`); SQLite/tests: the same statements in one transaction + a remaining-count check | owner (`m.owner = ?`) |
| Kill rewards | `clans_main.bank` +3/−2, clamped | §4 |

`slots + slots_up * step` is the site's `effectiveSlots` (`Clans.php:361–377`); `step` comes from
`config.json` (`services.slots_step`, default 10, `roles_step` default 3 — the values recorded in
`Clans.php:349–359`).

### `clans_logs`

The game now writes the same rows as `Clans::addLog` (`Clans.php:1057–1065`):
`(clan_id, action, value, time(), 1)` with PHP `json_encode(..., JSON_UNESCAPED_UNICODE)`
(`/` escaped too). Actions: `join` and `request_accept` accept-invite outcomes (`join`), `exit`,
`kick` `{player, reason}`, `request`, `request_accept`, `request_cancel`. Disband deletes the
clan's logs like the site's cascade. Nothing is logged for actions the site does not log
(invite sent/declined).

### After every action

* affected online members' caches are reloaded from the DB in one pass; their scoreboard tags are
  refreshed;
* the affected players get a chat line (joined/left/kicked/invited/request result/disbanded).

### Scoreboard tag

`guild.show_tag_on_scoreboard` now defaults **on**; the text is `ClanTag_Format` (default `{1}` —
the CS2 scoreboard already renders `m_szClan` inside brackets). The tag is trimmed and **one**
matching outer wrapper is stripped first (`[ADM]`, `(ADM)`, `{ADM}`, `<ADM>` → `ADM`), then the
format is applied, so a site tag stored with brackets is not doubled into `[[ADM]]`. Reapplied on
load, `player_spawn`, `player_team` and the refresh timer. Players without a clan are never
written to; the tag is cleared exactly once when they leave, are kicked, or their clan is
disbanded.

### Site-only (unchanged by v3)

Clan type changes, role create/edit/delete/assign, slot/role/upgrade purchases, duration and
extensions, name/desc/tag settings, exp/levels, services, reactions/views, and the site's own log
entries. The plugin never writes those columns.

### Tests (engine-free, 384 checks)

Rights per `getClanAccessCheck`, menu models per role, the invite registry (expiry, sender
cooldown, target disconnect, guild removal, "target joined another clan meanwhile"), the
join/apply/accept decision helpers, every action SQL builder and guard, the site-order disband
cascade (+ MySQL
single-statement form), PHP-compatible log JSON (including `\/`, `\u0001`, U+2028/U+2029), and an
SQLite smoke test of the store against a schema-only copy of `clans_main`/`clans_players`/
`clans_roles`/`clans_upgrade`/`clans_request`/`clans_logs`/`clans_reactions`/`clans_views`.

---

## 1c. Live website data (v4)

The user supplied the live module's cache/config files
(`.scratch/ref-k4/clans-site2/module_page_clans2`):

* **`levels.json`** — 6 levels by `clans_main.exp`: 0, 1000, 2000, 3000, 4000, 5000; names are
  null except level 6 = `Legend`. The plugin ships them as the default `config.json` `levels`
  array; a null/empty name is displayed as the localized `Level N` / `Уровень N`
  (`k4.menu.level.default`). `DeriveLevel` returns the level id and leaves the name empty so the
  UI can localize the fallback.
* **`services/items.json`** — `slots` value **10** (price 100, multiplier 1.5, max 10), `roles`
  value **3** (same price/mult/max), `duration`/`bank` disabled, `rating` +10000 exp for 150.
  This confirms the plugin's effective-cap formula: `slots + slots_up * 10` and
  `role_count + roles_up * 3` (`Clans.php:361–377`; config `services.slots_step`/`roles_step`
  default 10/3). Prices/multipliers/max and every purchase remain site-only.
* **`create.php`** — new clans start with **slots 20**, **roles 3**, bank 0, exp 0 (the site
  creates clans; the plugin never inserts a clan).
* **`logs.php`** — live logging configuration: master switch `on = '0'`, `delete_logs = '0'`,
  per-action flags `create/join/request/exit/kick/request_accept/request_cancel/settings/
  services/role_* = '1'`, `count_logs = 30`. The plugin follows this via `config.json` `logs`:
  it writes a `clans_logs` row only when `logs.on` is true **and** that action's flag is true
  (defaults: off + flags on, so **nothing is logged by default**). `delete_logs` controls whether
  a disband removes the clan's logs; the live value is off, so **disband keeps them by default**.
  (The older PHP copy in `clans-site/` had an ungated `addLog`; the live config is
  authoritative, and the rule is applied in the plugin as requested.)
* **`data/<clan_id>.json`** — the site's per-clan presentation cache (`detail_desc`, banners);
  not used by the game.

---

## 2. What the plugin does / does not do

| Area | Plugin (in-game runtime + menu) | Site |
|---|---|---|
| Clan create, settings, type, desc/tag/name, exp/levels | — | owns |
| Roles create/edit/delete/assign | — | owns |
| Services/purchases, duration/extensions | — | owns |
| Join (open) / apply / cancel request | menu mirrors `ClanJoin` SQL + log `join` / `request` / `request_cancel` | also supports them from the site |
| Invite / accept / decline | in-game only (in-memory) + `join` log on accept | no site counterpart |
| Leave / kick | menu mirrors `ClanExit` / `ClanPlayerKick` + `exit` / `kick` logs | also supports them from the site |
| Request accept/decline | menu mirrors `ClanRequestAccept` / `ClanRequestCancel` + logs | also supports them from the site |
| Disband | owner double-confirm; mirrors `deleteClanCascade` (logs included) | also supports it from the site |
| `clans_main.bank` | **+kill_reward / −death_penalty** (guarded, clamped) | spends/adds via services |
| `clans_players.kills`, `.deaths` | increments from `player_death` | reads/displays |
| `clans_players.name`, `.lastactivity` | refresh on connect / on kill events | reads/displays |
| `clans_main.name/tag/desc/type/duration/exp/slots/role_count/owner` | never written | writes |
| `clans_roles`, `clans_upgrade` | never written | writes |
| `clans_main.bank` (purchases) | never written except kill rewards | writes |
| `clans_logs` | writes the game-side action rows only when `logs.on` + the action flag are true (live: off); `delete_logs` gates removal on disband (live: keep) | writes/renders |
| Clan tag on scoreboard | default **on** (`ClanTag_Format`), live-check field | n/a |
| Guild chat + read-only commands | `help`, `info`, `members`, `top`, `chat` | n/a |
| Perks / services with game effects | **none**: the site's handler only implements slots/roles/rating/bank/duration, all site-side; unknown keys only charge/log (`data.php:979–993`) | sells |
| Additive `clans_k4_*` tables | **none needed** | n/a |

Dropped from the K4 port (with reasons):

* Site-only flows: creation, rename/settings/type/desc, role CRUD/assignment, slot/role/upgrade
  purchases, duration extensions, exp/levels, services, reactions/views.
* K4 Economy features (wallets, deposit/withdraw) — no economy plugin; the site owns the bank.
* K4 `BankCapacity`/`XPBoost`/`BankInterest` — no columns and no game consumer.
* K4 perks, `clans_k4_perks`, the perk export API — no site service sells or applies them; the
  site's buy handler has no game hook (`data.php:985–993`). If a game-effect service is ever
  added to `items.json`, it will need a site-side hook first.
* K4 global config ranks / `clans_k4_role_perms` — roles are site-owned; rights are derived from
  `clans_roles` exactly like the site.
* Command-level permission checks/menus of K4 — replaced by the site's role rights above.

---

## 3. Runtime data model (read-only loads)

The plugin loads a `Guild` snapshot for display/chat/accounting:

* `clans_main`: `id, name, tag, desc, owner, bank, slots, type, exp, role_count, created,
  lastupdate` (never `duration` for logic).
* `clans_players`: `id, auth, name, clan_id, kills, deaths, joined, lastactivity, role_id`.
* `clans_roles`: `id, clan_id, name, type, kick, roles, invite, settings` (other flags ignored).
* `clans_upgrade`: `slots_up, roles_up` (for the effective-cap display only).
* `clans_main.bank` is `int(11)`; all SQL keeps it in `[0, 2147483647]`.
* `auth`/`owner` are SteamID64 decimal strings (verified by the lead from the live dump); the
  plugin compares the s2sdk SteamID64 string form.

No `ALTER`/`DROP`/`RENAME` is ever executed; there are no `clans_k4_*` tables.

---

## 4. Kill/death accounting

Old config (`kill_rewards.ini`): `kill_reward 3`, `death_penalty 2`, `ignore_warmup 1`.

Rules implemented:

* `player_death` `Pre` hook, only with a live map (the B1/B2 map gate).
* Skip bots, unauthenticated slots, and the warmup window when `ignore_warmup` is on
  (`CCSGameRules::m_bWarmupPeriod`, the AntiAfk/RestrictedWeapons pattern).
* Killer's clan: `+kill_reward` **only** when the victim is a real enemy:
  attacker and victim valid, different players, different teams. Same-team (teamkill) kills pay
  nothing (recommended; the old config has no same-team option). An optional
  `ignore_same_clan` can also suppress rewards for clanmates on opposite teams.
* Victim's clan: `−death_penalty` for every non-bot death; the SQL clamps at 0
  (`CASE WHEN bank >= ? THEN bank - ? ELSE 0 END`), so the bank can never go negative.
* Stats: `kills = kills + 1` for a counting kill, `deaths = deaths + 1` for every non-bot
  victim; `lastactivity = now` for both.
* `clans_main.exp` is never touched (site level/rating system).
* SQL builders (unit-tested, portable MySQL/SQLite):
  * add: `UPDATE clans_main SET bank = CASE WHEN bank > 2147483647 - ? THEN 2147483647 ELSE bank + ? END WHERE id = ?`
  * subtract: `UPDATE clans_main SET bank = CASE WHEN bank >= ? THEN bank - ? ELSE 0 END WHERE id = ?`
  * kills/deaths: `UPDATE clans_players SET kills = kills + 1, lastactivity = ? WHERE auth = ?`
* Bank updates are plain atomic statements; no read-modify-write, so they interleave safely
  with the site's own `bank` statements.

Clan id for the attacker/victim comes from the per-player membership cache; a cache miss falls
back to `SELECT clan_id FROM clans_players WHERE auth = ? ORDER BY id LIMIT 1`.

---

## 5. Membership cache, tag, chat, levels, commands

* **Cache:** loaded on `player_activate`, reloaded on map start, and refreshed every
  `membership.refresh_seconds` (default 30) because the site may change anything at any time.
  Commands that display clan details load the guild fresh from the DB.
* **Clan tag:** `guild.show_tag_on_scoreboard` defaults **on**; the text is `ClanTag_Format`
  (default `{1}` — the CS2 scoreboard adds the brackets itself, and one stored outer
  `[]`/`()`/`{}`/`<>` wrapper is stripped before the format is applied). Write
  `CCSPlayerController::m_szClan` through the raw `s2sdk::SetEntDataCString` writer (size -1 =
  CUtlSymbolLarge, PITFALLS C3) on load, `player_spawn`, `player_team` and the refresh
  accumulator (no s2sdk timer). Players without a clan are never written to; the tag is
  cleared exactly once when they leave / are kicked / the clan is disbanded. Field name/offset
  must be verified live.
* **Chat prefix:** the old Metamod config has no chat-prefix setting (only command aliases
  `!clan,/clan,.clan,!clans` and console names `clans,clan_menu`), so no chat prefix is added.
  Guild chat (`/gc`, `/clan chat`) is a runtime feature (`chat.guild_chat`, default true); it
  sends to online clan members and writes nothing.
* **Levels:** the site computes the level from `clans_main.exp` and `levels.json` in its cache
  (`Clans.php:879–907`); that file is not in the DB. The plugin ships an optional `levels` array
  in `config.json` (same shape as `levels.json`) for the read-only `!clan info` and menu display.
  Without it the plugin shows the raw exp (level name "Level 1", 100% — the site's own
  empty-levels fallback).
* **Commands and menu:**
  * Bare `!clan` / `/clan` / `.clan` / `!clans` (and console `clans`, `clan_menu` for the text
    help) open the menu described in §1b; `.clan` via the `say`/`say_team` pre-listener
    (s2sdk only dispatches its configured `!`/`/` triggers).
  * `!clan info` / `!clan status` — name/tag, members, bank, type, exp + level, your role.
  * `!clan members` — members with role names, kills/deaths, online marker.
  * `!clan top <bank|exp|kills|deaths>` — clan tops (config-gated).
  * `!clan chat <text>` and `/gc <text>` — guild chat.
  * `!clan help`.
  * The read-only text commands stay available next to the menu.
* **Rights:** management menu items and every action re-check the site's rights (§1b).
  Guild chat has no site flag and is open to every member.
* **Writes:** the runtime accounting columns in §4 plus the site-mirror action SQL in §1b.

---

## 6. Logs

The site's `addLog` writes `(clan_id, action, value JSON, created, status=1)`
(`Clans.php:1057–1065`). Known action keys written by the module: `create`, `create_price`,
`exit`, `kick`, `role_change`, `transfer_owner`, `role_create`, `role_edit`, `role_delete`,
`request_accept`, `request_cancel`, `request_accept_all`, `request_cancel_all`, `settings`,
`join`, `request`, `services` (`data.php:304,306,365,395,434,474,511,564,591,635,649,688,701,
773,851,859,994`). Its admin view renders `action` and the JSON `value` verbatim
(`data.php:1151–1172`).

Since v3 the game performs site actions, so the plugin writes the same rows the site would write
for them: `join` (open join and accepted invite), `exit`, `kick` `{player,reason}`, `request`,
`request_accept`, `request_cancel`, with `value` produced like PHP
`json_encode(..., JSON_UNESCAPED_UNICODE)` (including `\/` and U+2028/U+2029 escaping),
`created=time()`, `status=1`.

v4 follows the live `logs.php` gates (`config.json` `logs`): a row is written only when the master
switch `on` is true **and** the action's own flag is set. The live values are `on=0` and all
action flags `1`, so the plugin writes **nothing by default**; enabling `on` reproduces the
site's per-action behaviour. `delete_logs` (live `0`) controls whether a disband deletes the
clan's log rows — off by default, so logs survive a disband. Invite sent/declined has no site
action and is never logged; kill/death accounting is not logged (too noisy).

---

## 7. Plugify mapping

* Plugin `k4_guilds`, entry `bin/k4_guilds`; dependencies `s2sdk >= 3.0.0` and the **optional**
  `hudmenu` (the player's own `!menu` choice; the menu works with the builtin types without it).
  No permissions/VIP dependencies, no exported methods. Standalone: no `pisexcompat` and no
  helper library (`docs/PLUGIN_STANDARD.md` §7); `libs/plgdb` is the only shared library.
* `plgdb` MySQL against the `clans` database; connection from
  `<configs>/k4_guilds/databases.cfg` section `k4_guilds` (Pisex KV, like `k4_missions`), with
  the pisexcompat lookup order (`addons/configs/databases.cfg` raw, then the plugin config dir,
  its `translations/` and the plugin data dir). A missing file/section writes one error line and
  fails soft to `data/k4_guilds/k4_guilds.sqlite3` (PLUGIN_STANDARD §4); SQLite is also the
  explicit test/offline driver. The production file is always present, so no data is forked.
* Native API instead of `pisexcompat`:
  `AddConsoleCommand`/`AddCommandListener` for the chat/console aliases (server console strictly
  `caller == -1`, client console/chat `0..63`, the `.clan` spelling caught by a `say`/
  `say_team` pre-listener), `HookEvent(..., Pre)` for the five events (player fields via
  `GetEventPlayerSlot`, every hook answers `Continue`), deadline accumulators in
  `OnPluginUpdate(dt)` for the membership refresh, tag refresh and invite expiry (no
  `s2sdk::CreateTimer`), the s2sdk menu API for the tree in §1b, the plugin-local KV1 reader and
  phrase/colour tables for the production files, and `s2sdk::PrintToChat` with the cs2-menus
  colour bytes.
* Events re-validate the slot at fire time; no client/schema calls before the map gate
  (`OnMapStart`/`OnMapEnd` plus the first-update latch);
  everything is removed in `OnPluginEnd` (`plg reload`).
* Warmup read: `GetSchemaOffset("CCSGameRules","m_bWarmupPeriod")` (resolved only with the map
  gate open, cached when >= 0) + `s2sdk::GetGameRules()` + `GetEntData2` (1 byte), the same field
  `plugins/AntiAfk` reads.
* Config `config.json`: command aliases, kill rewards, cache refresh, scoreboard tag, tops,
  service step values, guild-chat toggle, invite expiry/cooldown, optional levels.

Layout:

```
plugins/k4_guilds/
  CMakeLists.txt, k4_guilds.pplugin.in, sym/version_script.lds, package.sh
  src/{plugin.cpp,k4_core.cpp,k4_core.hpp,k4_files.cpp,k4_files.hpp,kv1.cpp,kv1.hpp,
       k4_store.cpp,k4_store.hpp}
  assets/{config.json,databases.cfg.example}
  translations/k4_guilds.phrases.txt
  tests/{k4_guilds_tests.cpp,k4_guilds_plugin_tests.cpp,k4_guilds_package_tests.cpp}
```

---

## 8. Open questions (only the user can answer)

1. **Teamkill/same-team rewards.** The old `kill_rewards.ini` has no switch. Recommendation:
   no bank reward and no `kills++` for same-team kills; the victim still gets `deaths++` and the
   clan still pays the death penalty. Confirm.
2. **Guild chat.** Keep `/gc` as a runtime-only feature (K4 parity, no DB writes), or drop it
   because the site does not know about it? Recommendation: keep, `chat.guild_chat` toggle.
3. **Invite defaults.** The old plugin had no invite expiry/cooldown settings in the provided
   config. Recommendation: 60 s expiry / 10 s sender cooldown (`config.json` `invites`), review
   on the live server.
4. **Logging defaults.** The live `logs.php` has the master switch off. The plugin therefore
   writes no `clans_logs` rows until `logs.on` is enabled. Confirm that the live site really
   gates `addLog` on `on` (the provided older PHP copy did not).

---

## 9. Live-check list

* `CCSPlayerController::m_szClan` (tag, now on by default) — field name/offset on the current
  build; verify the tag appears/changes/clears with leave/kick/disband.
* Menu rendering through hudmenu for a `hud`, `wasd`, `button`, `chat` and `centerhtml` player
  choice, and the page turn (8/9) on lists > 5 items (clans, members).
* `CCSGameRules::m_bWarmupPeriod` offset on the current build.
* `player_death` (Pre) delivers usable `attacker`/`userid` slots through `GetEventPlayerSlot`, and
  `k4::TeamOf` (controller `m_iTeamNum`) reflects the teams at event time.
* DB user has SELECT on `clans_*` and the writes used by the actions: UPDATE
  `clans_main.bank`, `clans_players.kills/deaths/name/lastactivity`; INSERT/DELETE
  `clans_players`/`clans_request`; DELETE `clans_main`/`clans_reactions`/`clans_views`/
  `clans_logs`; INSERT `clans_logs`.
* With the site creating/kicking members while the server runs, the 30 s refresh picks changes up
  and no stale clan id is used after a reconnect.
* Bank never negative after interleaved site spends and plugin death penalties.
* With `logs.on` enabled: every menu action creates the same `clans_logs` row as the site and
  appears in the admin log view (raw action/value); with the shipped `on=false` nothing is
  written. Verify the site's own `addLog` gating on the live PHP.
* Double-confirm disband removes the clan and reactions/views; logs are removed only when
  `logs.delete_logs` is on (live off). Every online member's tag and cache is cleared.
* Level display with the shipped 6-level table: 1000/2000/… thresholds and `Legend` at 5000;
  unnamed levels show "Level N"/"Уровень N".
* Effective slots for a fresh site clan (base 20) and after purchases (`+10` each); roles
  (`+3` each) match the site's `effectiveSlots`/`effectiveRoleCount`.

---

## 10. Evidence index

Site (`.scratch/ref-k4/clans-site/module_page_clans2/`):

* creation `data.php:128–309`; type setting `:735–747`; delete `:320–341`; exit `:343–367`;
  kick `:369–397`; role assignment `:399–436`; transfer `:438–476`; roles CRUD `:478–593`;
  requests `:595–703`; settings `:705–776`; reactions `:778–806`; join `:808–864`;
  services `:866–996`; logs admin view `:1151–1172`; levels admin `:1090–1149`.
* `Clans.php:52–56` DB list (`clans` etc.); `:291–306` type numbering (1 open, 2 closed,
  3 requests); `:317–333` role types; `:349–359` serviceStep (slots 10/roles 3); `:361–377`
  effective slots/roles; `:656–661` member ordering by kills; `:801–823` access check;
  `:879–907` level derivation; `:930–941` lastactivity display; `:1057–1065` `addLog`;
  `:1091–1107` delete cascade.

Old config: `kill_rewards.ini` (+3/−2/ignore warmup), `core.ini` (aliases), `upgrade.ini`
(slots/roles prices/incs — all site-owned now), `top.ini` (tops), `rconcommand.ini` (defaults).

Live schema: `.scratch/ref-k4/clans-db/schema.sql` (clans_main :37–51, clans_players :54–70,
clans_roles :107–121, clans_upgrade :125–130, clans_logs :27–35, clans_request :95–104,
clans_reactions :84–93, clans_views :133–141; `clans_players.auth` has no unique key).

Port infrastructure: `libs/plgdb/include/plgdb/db.hpp`; `docs/PLUGIN_STANDARD.md`;
`handoff/weaponskins/PITFALLS.md`; `plugins/k4_missions` (manifest/CMake/DB patterns),
`plugins/quake_sounds` and `plugins/as_maps` (native menu/command/map-gate patterns).
