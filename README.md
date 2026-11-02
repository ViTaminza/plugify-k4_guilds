# k4_guilds (Plugify)

In-game runtime and clan menu for the CS2 clan system whose database is owned by the clans2 website module (module_page_clans2). The site owns clan settings/type, roles, services, levels and duration; the plugin maintains the runtime columns the site displays (clans_main.bank from kill rewards, clans_players.kills/deaths/name/lastactivity) and implements the old Metamod in-game management menu with the native s2sdk menu API: clan list/info, join or apply (and cancel) depending on the clan type, pending invites, members/member cards, tops, guild chat, invite (online player list, accept/decline menu, expiry + cooldown), kick, join requests accept/decline and owner double-confirm disband. The optional hudmenu plugin supplies the player's own '!menu' choice; without it the menu uses the s2sdk builtin types. Actions mirror the site's SQL with race-safe guards and write the same clans_logs rows (join/exit/kick/request/request_accept/request_cancel); disband cascades like Clans::deleteClanCascade. Scoreboard clan tag defaults on. No existing table or column is altered and no additive tables are used. Configuration: config.json and databases.cfg (section k4_guilds, MySQL; SQLite is the fail-soft/test driver).

Plugify port (fork) of **K4 Guilds** by K4ryuu.
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 2.0.0
- Plugify dependencies: s2sdk (>=3.0.0), hudmenu (>=1.0.0, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `k4_guilds-2.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/k4_guilds`.

**Testing status: NOT TESTED IN GAME. / Не тестировался в игре.** Standalone compilation and package checks do not validate gameplay, database, Discord or external service integrations.

Requires the clans2 website database schema; an empty database alone is not sufficient.

## License
GPL-3.0, see LICENSE.

