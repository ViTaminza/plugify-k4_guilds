// k4_guilds — engine-free core of the in-game runtime for the site-owned clans_* database.
//
// The website module (module_page_clans2) owns every clan/role/membership/economy/level/log
// flow. The plugin only maintains the runtime columns the site displays (clans_main.bank from
// kill rewards, clans_players.kills/deaths/name/lastactivity) and offers read-only views.
//
// Nothing here includes s2sdk, plugify or plgdb: config parsing, level derivation, kill/death
// accounting, the clamped SQL builders and command parsing are unit-tested without a server.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace k4 {

// ---------------------------------------------------------------------------------------------
// Configuration (config.json)
// ---------------------------------------------------------------------------------------------

// One entry of the site's levels.json (name/exp thresholds), copied into the plugin config for
// the read-only level display. The site keeps the authoritative file in its cache.
struct ClanLevel {
	int id = 1;
	std::string name;
	int64_t exp = 0;
};

// Site logs.php gate: master switch `on`, per-action flags, `delete_logs` controls whether a
// disband removes the clan's log rows (live values: on=0, delete_logs=0, flags=1).
struct LogConfig {
	bool on = false;
	bool deleteLogs = false;
	bool join = true;
	bool request = true;
	bool exit = true;
	bool kick = true;
	bool requestAccept = true;
	bool requestCancel = true;
};

struct Config {
	// Old core.ini aliases.
	std::vector<std::string> chatAliases = {"!clan", "/clan", ".clan", "!clans"};
	std::vector<std::string> consoleAliases = {"clans", "clan_menu"};

	// Old kill_rewards.ini.
	int killReward = 3;
	int deathPenalty = 2;
	bool ignoreWarmup = true;
	bool ignoreSameTeam = true; // recommended: a teamkill pays no clan-bank reward
	bool ignoreSameClan = false;
	// Bots have no SteamID and no clan; by default any death a bot took part in is ignored
	// entirely (DESIGN §4 "Skip bots"). true restores the old per-participant behaviour.
	bool countBots = false;

	// The site may change membership at any time; reload the per-player cache this often.
	int membershipRefreshSeconds = 30;

	// The old plugin showed the clan tag; live-check the client field on deployment.
	bool showTagOnScoreboard = true;
	int scoreboardRefreshSeconds = 60;

	// In-game invites (in-memory, old-plugin semantics).
	int inviteExpireSeconds = 60;
	int inviteCooldownSeconds = 10;

	// Old top.ini.
	bool topBank = false;
	bool topExp = true;
	bool topKills = true;
	bool topDeaths = true;

	// The site computes effective slots/roles as base + purchases * serviceStep (Clans.php:361-377).
	// serviceStep reads the site's items.json; the module records slots=10, roles=3 (Clans.php:349-359).
	int slotsStep = 10;
	int rolesStep = 3;

	// Guild chat is runtime-only (no DB writes).
	bool guildChat = true;

	// Optional copy of the site's levels.json for the in-game level display. Empty = show exp.
	std::vector<ClanLevel> levels;

	// clans_logs writing (site logs.php semantics).
	LogConfig logs;
};

// True when the master switch is on and the action's own flag is set.
bool ShouldLogAction(const Config& config, std::string_view action);

// Parses config.json (JSONC comments accepted, property names case-insensitive). Returns false
// only for malformed JSON / a non-object root; a wrong property type is reported in `error` but
// the defaults stay usable.
bool ParseConfig(std::string_view text, Config& out, std::string& error);

// ---------------------------------------------------------------------------------------------
// Live DB model (read-only)
// ---------------------------------------------------------------------------------------------

struct Role {
	int32_t id = 0;
	int32_t clanId = 0;
	std::string name;
	int32_t type = 3; // 1 = owner role, 2 = participant, 3 = custom (Clans.php:317-333)
	bool kick = false;
	bool roles = false;
	bool invite = false;
	bool settings = false;
};

struct Member {
	int64_t id = 0;
	int32_t clanId = 0;
	std::string auth; // SteamID64 decimal
	std::string name;
	int32_t roleId = 0;
	int32_t kills = 0;
	int32_t deaths = 0;
	int64_t joined = 0;
	int64_t lastactivity = 0;
};

struct Guild {
	int32_t id = 0;
	std::string name;
	std::string tag;
	std::string desc;
	std::string ownerAuth;
	int32_t bank = 0;
	int32_t slots = 0;
	int32_t type = 1;
	int32_t exp = 0;
	int32_t roleCount = 0;
	int64_t created = 0;
	int64_t lastupdate = 0;
	int32_t slotsUp = 0; // clans_upgrade.slots_up (display only)
	int32_t rolesUp = 0; // clans_upgrade.roles_up (display only)
	std::vector<Member> members;
	std::vector<Role> roles;
};

const Member* FindMember(const Guild& guild, std::string_view auth);
const Member* FindMemberBySteam(const Guild& guild, uint64_t steamid);
const Role* FindRole(const Guild& guild, int32_t roleId);
bool IsOwner(const Guild& guild, std::string_view auth);

// A freshly loaded guild, handed to the deferred request lookup: the id and the retained
// shared_ptr are read/created in one step. Do NOT inline this into a call expression like
//   HasPendingRequest(guild->id, auth, [guild = shared_ptr(std::move(guild))](bool) {...})
// - C++ leaves argument evaluation order unspecified and GCC builds the lambda (moving the
// unique_ptr) first, so guild->id then dereferences a null pointer (live crash at Guild::id,
// offset 0). Null guild -> id 0, null shared_ptr.
struct GuildHandoff {
	int32_t guildId = 0;
	std::shared_ptr<Guild> guild;
};
GuildHandoff TakeLoadedGuild(std::unique_ptr<Guild> guild);

// clans_main.type: 1 = open, 2 = inviteonly (closed), 3 = request (Clans.php:291-306).
const char* GuildTypeName(int type);

// --- rights (site rules, Clans::getClanAccessCheck, Clans.php:801-823) ----------------------
//
// The owner has every right. A non-owner member gets:
//   Invite / Requests <- clans_roles.invite   (the site uses 'invite' for request accept/cancel)
//   Kick              <- clans_roles.kick
//   Settings          <- clans_roles.settings
//   Roles             <- clans_roles.roles     (site-only role editor; no in-game action)
//   Disband/Services  <- owner only
// A member with a NULL/foreign role has no rights.
enum class ClanRight {
	Invite,
	Kick,
	Requests,
	Disband,
	Settings,
	Roles,
	Services,
};

bool HasClanRight(const Guild& guild, std::string_view auth, ClanRight right);

// --- menu models (unit-tested) ---------------------------------------------------------------

struct MemberMenuModel {
	bool info = true;
	bool members = true;
	bool top = true;
	bool management = false;
	bool invite = false;
	bool kick = false;
	bool requests = false;
	bool disband = false;
	bool leave = false;
};

MemberMenuModel BuildMemberMenuModel(const Guild& guild, std::string_view auth);

enum class JoinAction {
	None,
	JoinOpen,
	Apply,
	CancelRequest,
	InviteOnly,
};

JoinAction DetermineJoinAction(int type, bool inClan, bool hasPendingRequest);

// --- invites (in-memory, old-plugin semantics) ------------------------------------------------

struct ClanInvite {
	int32_t guildId = 0;
	uint64_t inviter = 0;
	uint64_t target = 0;
	int64_t expiresAt = 0;
};

class InviteRegistry {
public:
	void Configure(int expireSeconds, int cooldownSeconds);
	int ExpireSeconds() const { return m_expireSeconds; }
	int CooldownSeconds() const { return m_cooldownSeconds; }

	// false when the inviter is still in cooldown. Replaces an existing invite for the target.
	bool Create(int32_t guildId, uint64_t inviter, uint64_t target, int64_t now);
	std::optional<ClanInvite> Get(uint64_t target, int64_t now);
	bool Accept(uint64_t target, int32_t guildId, int64_t now);
	void Decline(uint64_t target);
	void RemoveTarget(uint64_t target);
	void RemoveGuild(int32_t guildId);
	// Drops expired invites (and stale cooldowns); returns the number of removed invites.
	std::size_t Cleanup(int64_t now);
	bool CooldownActive(uint64_t inviter, int64_t now) const;
	std::size_t Size() const { return m_invites.size(); }

private:
	std::map<uint64_t, ClanInvite> m_invites;
	std::map<uint64_t, int64_t> m_lastInvite;
	int m_expireSeconds = 60;
	int m_cooldownSeconds = 10;
};

enum class InviteAcceptResult {
	Ok,
	Expired,
	ClanGone,
	AlreadyInClan,
	Full,
};

// The accept decision, including the "target joined another clan meanwhile" case.
InviteAcceptResult EvaluateInviteAccept(InviteRegistry& invites, uint64_t target, int32_t guildId,
										bool targetInClan, bool clanExists, int32_t memberCount,
										int32_t effectiveSlots, int64_t now);

// Effective caps as the site computes them (base + purchases * step).
int32_t EffectiveSlots(const Guild& guild, int slotsStep);
int32_t EffectiveRoles(const Guild& guild, int rolesStep);

// ---------------------------------------------------------------------------------------------
// Levels (mirrors Clans::getClanLevelProgress, Clans.php:879-907)
// ---------------------------------------------------------------------------------------------

struct LevelInfo {
	int id = 1;
	std::string name; // empty when the site's levels.json has a null name
	int64_t currentExp = 0;
	int64_t requiredExp = 0;
	int percent = 100;
};

LevelInfo DeriveLevel(int64_t exp, const std::vector<ClanLevel>& levels);

// ---------------------------------------------------------------------------------------------
// Kill/death accounting
// ---------------------------------------------------------------------------------------------

struct KillDeltas {
	bool attacker = false;
	int32_t attackerDelta = 0;
	bool victim = false;
	int32_t victimDelta = 0;
};

// +kill_reward to the killer's clan, -death_penalty to the victim's clan.
// A reward is skipped for warmup (when ignoreWarmup), invalid/bot participants, suicides and
// same-team kills (when ignoreSameTeam; optionally same-clan kills too). The victim penalty is
// applied for every valid victim; the SQL clamps the bank at 0.
KillDeltas ComputeKillDeltas(const Config& config, bool attackerValid, bool victimValid, bool samePlayer,
							 bool sameTeam, bool sameClan, bool warmup);

// True when the death event must be ignored because a bot took part and count_bots is off.
bool IgnoreBotDeath(bool countBots, bool attackerBot, bool victimBot);

// The int(11) bank column is clamped to [0, INT32_MAX].
int32_t ClampBankValue(int64_t current, int64_t delta);
bool BankDeltaApplies(int64_t current, int64_t delta);

// Parameterised SQL shared by the store and the tests. All statements are single atomic
// statements (no read-modify-write), so they are safe against the site's concurrent writes.
std::string_view BankAddSql();
std::string_view BankSubtractSql();
std::string_view KillsIncrementSql();
std::string_view DeathsIncrementSql();
std::string_view TouchMemberSql();	  // name + lastactivity
std::string_view TouchActivitySql(); // lastactivity only

// Menu actions (mirror the site's SQL, with race-safe guards). `mysql` selects the
// "FROM DUAL" form where a SELECT of constants is used; SQLite omits it.
std::string JoinClanSql(bool requireOpenType, bool mysql);
std::string ApplyRequestSql(bool mysql);
std::string_view LeaveClanSql();
std::string_view KickMemberSql();
std::string_view AcceptRequestSql(); // same SQL on MySQL and SQLite (selects FROM clans_request)
std::string_view DeleteRequestByIdSql();
// The request row goes only when the accepted membership exists: run inside the same
// transaction as AcceptRequestSql(), this guarded delete is also the success signal (plgdb
// reports only the last statement's result; a refused insert leaves nothing to delete).
std::string_view DeleteAcceptedRequestSql();
std::string_view DeleteRequestByPlayerSql();
std::string_view DisbandMainSql();
// MySQL: the first statement of the disband transaction locks the clan row (FOR UPDATE), so the
// owner guard cannot be invalidated by the site's concurrent writes. SQLite has no FOR UPDATE;
// its transaction takes the write lock with the first cascade DELETE instead.
std::string_view DisbandLockSql();
// The site's deleteClanCascade order (players, roles, request, reactions, views, upgrade, logs,
// main) as one transaction, followed by DisbandRemainingSql. Every cascade statement carries the
// owner guard too (plgdb cannot branch on a statement result, so a failed lock must make every
// delete a no-op); the last statement's remaining count tells whether the clan was removed.
std::vector<std::string_view> DisbandCascadeSql(bool mysql, bool deleteLogs);
std::string_view DisbandRemainingSql();
std::string_view InsertLogSql();

// clans_logs JSON exactly like PHP json_encode($value, JSON_UNESCAPED_UNICODE) for the site's
// arrays: {"player":"..."} and {"player":"...","reason":"..."}.
std::string JsonEncodePhp(std::string_view value);
std::string LogValuePlayer(std::string_view auth);
std::string LogValueKick(std::string_view auth, std::string_view reason);

// ---------------------------------------------------------------------------------------------
// s2sdk safety helpers (PITFALLS C1, C10, C11)
// ---------------------------------------------------------------------------------------------

// A s2sdk entity handle is an int32 view of the engine's uint32 CEntityHandle
// (index | serial << 15): 0 is the world and -1 is INVALID_EHANDLE_INDEX. Every other value may be
// a real handle, *including a negative one*, because once the engine's serial counter passes 65535
// bit 31 is set (PITFALLS C1). Never test a handle with `<= 0` or `> 0`; pass it to
// s2sdk::IsValidEntHandle() after this check.
bool IsValidEntityHandle(int32_t handle);

// pisexcompat's ResolveS2sdkMenuTypeName() for a module whose Pisex menu type is HUD_LAYOUT
// (k4_guilds never forces a type): the player's own "!menu" choice from the optional hudmenu
// plugin wins while its backend is registered, otherwise the HUD_LAYOUT default applies. The
// table is preserved exactly:
//   "chat"/"centerhtml"/"button" -> that builtin;
//   "hud"  -> "hud" when registered, otherwise "button" (never another custom type);
//   "wasd" -> "wasd" when registered, otherwise "button" (never "hud");
//   anything else (or no choice) -> "hud" when registered, otherwise "button".
std::string ResolveMenuTypeName(std::string_view playerChoice, bool hudRegistered,
								bool wasdRegistered);

// The chat triggers pisexcompat registered: '!' and '/' (s2sdk dispatches a chat line whose first
// character is a configured trigger as a command with the trigger stripped).
bool IsChatTrigger(char c);
// Strips one leading '!' or '/' (not the '.' alias: that one is caught by the say listener).
std::string StripChatTrigger(std::string_view name);

// ---------------------------------------------------------------------------------------------
// Commands (read-only)
// ---------------------------------------------------------------------------------------------

std::vector<std::string> TokenizeCommand(std::string_view line);

enum class Subcommand {
	Help,
	Info,
	Members,
	Top,
	Chat,
	Unknown,
};

Subcommand ResolveSubcommand(std::string_view input);
const char* SubcommandCanonical(Subcommand command);
const char* SubcommandAliases(Subcommand command); // documentation string, e.g. "info, status"

// ---------------------------------------------------------------------------------------------
// Phrases and helpers
// ---------------------------------------------------------------------------------------------

// The Pisex "{1}"/"{2}" substitution (args[0] replaces {1}).
std::string FormatPhrase(std::string_view phrase, const std::vector<std::string>& args);

// Scoreboard tag text. The CS2 scoreboard renders m_szClan in brackets itself, so the tag is
// trimmed and one matching outer wrapper ([] () {} <>) is stripped before the format is
// applied; with the default format "{1}", "[ADM]" and "ADM" both become "ADM".
std::string FormatClanTagText(std::string_view format, std::string_view tag);

std::string LowerAscii(std::string_view text);
bool EqualsIgnoreCase(std::string_view left, std::string_view right);
bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle);
std::string TrimSpaces(std::string_view text);

} // namespace k4
