// k4_guilds — in-game runtime for the site-owned clans_* database.
//
// The website module (module_page_clans2) owns clan creation/deletion, membership, roles,
// requests, settings, services, levels and logs. This plugin only:
//   * maintains clans_main.bank from kill rewards and clans_players.kills/deaths,
//   * refreshes clans_players.name/lastactivity,
//   * offers read-only commands (info/members/top) and guild chat,
//   * optionally writes the clan tag to the scoreboard (config flag, default on),
//   * implements the old Metamod in-game clan menu through the native s2sdk menu API.
//
// Standalone (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no helper library. The plugin's
// production KeyValues files are read by the plugin-local KV1 reader (src/kv1.cpp) through
// src/k4_files.cpp; the map gate, client/event/schema checks, native menus, commands and the
// database live in this file. The only optional plugin dependency is the runtime hudmenu API,
// used for one thing: the player's own "!menu" choice (GetClientMenuType), resolved with
// pisexcompat's ResolveS2sdkMenuTypeName table (k4::ResolveMenuTypeName).
//
// The guards of handoff/weaponskins/PITFALLS.md live in this file:
//
//   * A1/A7  OnPluginStart() never returns Failed (that stops every Plugify plugin). Every
//            registration is guarded by the presence of its complete register+unregister pair:
//            OnMapStart/OnMapEnd Register+Unregister (4 bindings), HookEvent+UnhookEvent,
//            AddConsoleCommand+RemoveCommand+RemoveCommandListener,
//            AddCommandListener+RemoveCommandListener. OnPluginEnd symmetrically removes every
//            command (RemoveCommand only for a name this plugin created, RemoveCommandListener
//            for a pre-existing one), every listener, every event hook, the map listeners and
//            every menu it created.
//   * B1/B2  the map gate g_mapReady. No s2sdk client, entity or schema call is made before the
//            first map or in the window between OnMapEnd and the next OnMapStart: OnMapStart
//            grants readiness, OnMapEnd drops it (state only, no client call), and a plugin
//            loaded into a running map latches it on the FIRST OnPluginUpdate - only when all four
//            map-listener bindings were registered, no OnMapEnd was seen since OnPluginStart and
//            s2sdk::GetCurrentMap() reports a real map (non-empty and not "<empty>").
//   * C1     a CEntityHandle is a uint32 view (index | serial << 15): only 0 (the world) and -1
//            (INVALID_EHANDLE_INDEX) are invalid, a negative handle is valid once the serial
//            counter passes bit 31. Every handle goes through k4::IsValidEntityHandle() and then
//            s2sdk::IsValidEntHandle(); this file has no `handle <= 0` / `handle > 0` test.
//   * C2     every client query checks 0 <= slot < 64, IsClientConnected, IsClientInGame,
//            !IsClientSourceTV, a valid controller pointer and a valid controller handle;
//            GetClientSteamID64 is only read after those checks (bots have none).
//   * C3/C8  schema offsets are resolved only with the gate open and only successful lookups are
//            cached (offset >= 0; chain offset > 0, otherwise -1); reads use the raw
//            GetEntData2/GetEntData with the right width (team = 1 byte, warmup = 1 byte) and the
//            clan tag goes through the raw CUtlSymbolLarge writer (size -1) on a validated handle.
//   * C9/B4  this plugin owns no s2sdk timer. The membership refresh, the scoreboard tag refresh,
//            the invite expiry and the deferred post-map reconcile are deadline accumulators
//            driven by OnPluginUpdate(dt); plgdb is pumped by Database::Poll() from the same
//            update. No action ever runs inside s2sdk's timer mutex.
//   * C10    native menus: MenuId 0 is reserved with a dummy menu before any real menu is
//            created, and the full teardown set (CreateMenu, DestroyMenu, DisplayMenu,
//            AddMenuItem, GetClientMenu, CancelClientMenu, SetMenuType, GetMenuType) is required
//            before the first CreateMenu. The menu handler runs under the menu manager's lock, so
//            it only queues: every selection is applied from OnPluginUpdate with the SteamID64 +
//            controller handle captured at display time (a reused slot, a reconnected player or a
//            swapped invite target can never act for/on somebody else). On a changelevel and on
//            unload every display is ended with CancelClientMenu(Disconnect), re-checked with
//            GetClientMenu, switched to a builtin no-op-close type ("chat"/"console", verified
//            with GetMenuType) when it still reports a display, and only then destroyed; ids that
//            s2sdk refuses to release are parked and finally reported as A7_UNRESOLVED.
//   * C11    commands: the console/chat aliases are registered with Release|ClientCanExecute and
//            answered only for a real source: server console strictly caller == -1, client console
//            or chat 0 <= caller < 64; any other caller is refused without running anything. The
//            "." alias is caught by the say/say_team pre-listener because s2sdk only dispatches
//            its configured triggers. The AddConsoleCommand ownership decides the teardown
//            (RemoveCommand vs RemoveCommandListener).
//   * events: game events are Pre hooks that always answer Continue. Player fields ("userid",
//            "attacker", "assister") are read with s2sdk::GetEventPlayerSlot; GetEventInt is
//            never used for a player field.
//
// Porting notes (Metamod/K4 -> native s2sdk):
//   * config.json keeps its plugin path (<configs>/k4_guilds/config.json) and format; the
//     databases.cfg / phrases.txt lookup order is pisexcompat's (see src/k4_files.hpp).
//   * the language chain is <configs>/core.cfg "ServerLang" -> s2sdk ServerLanguage -> "en";
//     chat output colourises the cs2-menus tags itself because s2sdk PrintToChat sends plain text.
//   * the Pisex menu tree is unchanged; the only difference is that items are rendered by the
//     native menu backend (hudmenu's "hud" by default, or the player's own choice).
//   * the original Pisex query order (clan list, guild load, request check) is preserved; the
//     membership cache and the reconcile pass are the previous port's.
//
// PITFALLS A6: OnPluginEnd does not run on a server quit, so nothing is only saved there -
// membership changes, kill rewards and logs are written when they happen.

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hudmenu/hudmenu.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>
#include <plgdb/db.hpp>
#include <s2sdk/s2sdk.hpp>

#include "k4_core.hpp"
#include "k4_files.hpp"
#include "k4_store.hpp"

namespace k4 {
namespace {

constexpr const char* kPluginName = "k4_guilds";
constexpr const char* kPhrasesFile = "k4_guilds.phrases.txt";
constexpr const char* kDatabasesCfg = "addons/configs/databases.cfg";
constexpr const char* kSqliteFile = "k4_guilds.sqlite3";
constexpr const char* kLogTag = "k4_guilds";
constexpr int kMaxSlots = 64;
// Backpressure: plgdb cannot drop queued work at shutdown, so the plugin must not let the single
// FIFO worker's queue grow without bound when the database is slow or unreachable. Above this
// many pending statements the periodic refresh and every DB-backed menu/command answer are
// skipped until the worker catches up.
constexpr std::size_t kMaxPendingStatements = 64;
// cs2-menus/Pisex showed five entries per page and numbered the visible ones 1..5.
constexpr int kMenuItemsPerPage = 5;
// pisexcompat's default menu timeout (g_menuTimeout = 60): a menu that is left alone closes.
constexpr int kMenuTimeoutSeconds = 60;
constexpr std::size_t kMaxQueuedActions = 256;
constexpr std::size_t kMaxQueuedMenuDestroys = 256;
// s2sdk's own defaults when GetMaxClients is unavailable.
constexpr int kDefaultMaxClients = 64;

// Forward declarations: the command, menu and event helpers are mutually recursive (a menu action
// opens another menu, a failed action reports and reopens the main menu).
void CmdHelp(int slot);
void CmdInfo(int slot, uint64_t steamid);
void CmdMembers(int slot, uint64_t steamid);
void CmdTop(int slot, const std::vector<std::string>& args);
void CmdChat(int slot, uint64_t steamid, const std::vector<std::string>& args);
void Dispatch(int slot, Subcommand command, const std::vector<std::string>& args);
void OpenMainMenu(int slot);
void HandleMenuAction(int slot, const std::string& action);
std::string FormatClanTag(const std::string& tag);
void ApplyScoreboardTag(int slot, const std::string& tag);

// ---------------------------------------------------------------------------------------------
// Logging and small conversions
// ---------------------------------------------------------------------------------------------

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(std::string(message), plg::Severity::Info, plg::source_location::current());
	}
}

void LogWarning(std::string_view message) {
	if (plg::Log) {
		plg::Log(std::string(message), plg::Severity::Warning, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(std::string(message), plg::Severity::Error, plg::source_location::current());
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

plg::string ToPlg(const std::string& text) {
	return plg::string(text.c_str());
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

Config g_config;
PhraseMap g_phrases;
std::string g_language = "en";

std::unique_ptr<plgdb::Database> g_database;
std::unique_ptr<GuildStore> g_store;

// One online player's membership cache (the site may change it at any time).
struct PlayerInfo {
	uint64_t steamid = 0;
	int slot = -1;
	int32_t clanId = 0;
	std::string auth;
	std::string clanTag;
	uint64_t generation = 0;
	bool loaded = false;
	bool loading = false;
};

// One player of a reconcile pass: the per-player clan-id lookup, then one shared guild load per
// distinct clan instead of a full five-query load (and roster re-read) per member.
struct ReconcileLoad {
	uint64_t steamid = 0;
	uint64_t generation = 0;
	std::string auth;
};

struct ReconcilePass {
	uint64_t id = 0;
	std::unordered_map<int32_t, std::vector<ReconcileLoad>> clans;
	std::size_t awaitingClanIds = 0;
	std::size_t awaitingGuilds = 0;
};

std::unordered_map<uint64_t, PlayerInfo> g_players;
std::array<uint64_t, kMaxSlots> g_slotSteam {};
uint64_t g_nextGeneration = 0;
bool g_reconcileInFlight = false;
uint64_t g_nextReconcilePass = 0;
uint64_t g_activeReconcilePass = 0;
std::chrono::steady_clock::time_point g_reconcileStartedAt {};
std::chrono::steady_clock::time_point g_lastLoadErrorLog {};
std::array<std::chrono::steady_clock::time_point, kMaxSlots> g_lastCommandAt {};
std::array<std::chrono::steady_clock::time_point, kMaxSlots> g_lastMenuActionAt {};

InviteRegistry g_invites;

// PITFALLS B1/B2: the map gate. `g_mapListeners` is true only when both register/unregister
// pairs were acknowledged by s2sdk; `g_firstUpdateDone` makes the mid-map latch a one-shot.
bool g_mapReady = false;
bool g_mapEndSeen = false;
bool g_mapListeners = false;
bool g_mapStartAttempted = false;
bool g_mapStartRegistered = false;
bool g_mapEndAttempted = false;
bool g_mapEndRegistered = false;
bool g_firstUpdateDone = false;

bool g_started = false;
bool g_needReconcile = false;
bool g_shuttingDown = false;

// Deadline accumulators (PITFALLS C9/B4): the plugin creates no s2sdk timer.
double g_refreshAccumulator = 0.0;
double g_tagAccumulator = 0.0;
double g_inviteAccumulator = 0.0;

// Lazy schema offsets: resolved only with the gate open, only successful values are cached
// (PITFALLS C8).
int g_warmupOffset = -1;
int g_clanOffset = -1;
int g_clanChainOffset = -1;
int g_teamOffset = -1;

// ---------------------------------------------------------------------------------------------
// Player identity and client queries
// ---------------------------------------------------------------------------------------------

// The identity of the player currently occupying a slot: the SteamID64 plus the controller's
// CEntityHandle. The handle carries the entity serial, so a reused slot - even one reused by the
// same SteamID on a new controller entity - is a different identity.
struct PlayerIdentity {
	uint64_t steamid = 0;
	int32_t controllerHandle = 0;
	bool valid = false;

	// The strict check used by every deferred action: a human SteamID64 and a real CEntityHandle
	// (never the handle==0 world value, never -1). A bot identity (steamid 0) is deliberately not
	// "valid" for actions; PlayerValid also rejects bots before this is reached.
	[[nodiscard]] bool Valid() const {
		return valid && steamid != 0 && IsValidEntityHandle(controllerHandle);
	}
};

bool ValidSlot(int slot) {
	return slot >= 0 && slot < kMaxSlots;
}

// ---------------------------------------------------------------------------------------------
// Map lifecycle (PITFALLS B1/B2, A7)
// ---------------------------------------------------------------------------------------------

void OnMapStartListener();
void OnMapEndListener();
void TeardownMenusOnMapEnd();

void UnregisterMapListeners() {
	if (g_mapStartAttempted && __s2sdk_OnMapStart_Unregister != nullptr) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		} catch (...) {
		}
	}
	g_mapStartAttempted = false;
	g_mapStartRegistered = false;
	if (g_mapEndAttempted && __s2sdk_OnMapEnd_Unregister != nullptr) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		} catch (...) {
		}
	}
	g_mapEndAttempted = false;
	g_mapEndRegistered = false;
	g_mapListeners = false;
}

void RegisterMapListeners() {
	if (g_mapListeners) {
		return;
	}
	// A7: a callback that cannot be unregistered would point into this library after unload. The
	// whole register/unregister pair of both listeners is required before anything is registered;
	// without the map-end listener the gate could never close again anyway.
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapStart_Unregister == nullptr ||
		__s2sdk_OnMapEnd_Register == nullptr || __s2sdk_OnMapEnd_Unregister == nullptr) {
		LogError(std::string("[") + kLogTag +
				 "] the s2sdk map listeners are unavailable (both register and unregister bindings "
				 "are required) - the runtime stays inactive");
		return;
	}
	// Mark the attempt before the call: if s2sdk registered the callback and then threw, the
	// unregister below still has to remove it. Only a successful call counts as registered, so a
	// half-registered pair keeps the gate shut.
	g_mapStartAttempted = true;
	try {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		g_mapStartRegistered = true;
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] could not register the OnMapStart listener");
	}
	g_mapEndAttempted = true;
	try {
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
		g_mapEndRegistered = true;
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] could not register the OnMapEnd listener");
	}
	g_mapListeners = g_mapStartRegistered && g_mapEndRegistered;
	if (!g_mapListeners) {
		UnregisterMapListeners();
	}
}

// gpGlobals->mapname is empty before the first map and "<empty>" for a level without a name; the
// mid-map latch may only open the gate for a real map (the host pumps OnPluginUpdate through the
// pre-map startup and through a reload in the changelevel window).
bool GameMapLoaded() {
	if (__s2sdk_GetCurrentMap == nullptr) {
		return false;
	}
	try {
		const std::string map = ToStd(s2sdk::GetCurrentMap());
		return !map.empty() && map != "<empty>";
	} catch (...) {
		return false;
	}
}

void PollMapGate() {
	if (g_firstUpdateDone) {
		return;
	}
	g_firstUpdateDone = true;
	if (g_mapReady || g_mapEndSeen || !g_mapListeners) {
		return;
	}
	if (!GameMapLoaded()) {
		return;
	}
	// A plugin loaded (or reloaded) into a running map never saw OnMapStart; its first update is
	// the proof of a running server.
	g_mapReady = true;
	g_needReconcile = true;
}

void OnMapStartListener() {
	// The engine calls this once the new map and its client list exist: every client call is legal
	// from here on. OnMapStart itself only changes state (no client/entity/schema call).
	g_mapReady = true;
	g_mapEndSeen = false;
	g_needReconcile = true; // the first update after the map start refreshes the cache
	g_refreshAccumulator = 0.0;
	g_tagAccumulator = 0.0;
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart the client list is torn down and any client
	// query crashes - only state changes happen here.
	g_mapReady = false;
	g_mapEndSeen = true;
	g_needReconcile = false;
	TeardownMenusOnMapEnd();
	g_slotSteam.fill(0);
}

// ---------------------------------------------------------------------------------------------
// Player identity and client queries
// ---------------------------------------------------------------------------------------------

bool HasController(int slot) {
	if (!g_mapReady || !ValidSlot(slot)) {
		return false;
	}
	// Fail closed (PITFALLS A7/C2): the controller pointer, its validator, the handle accessor and
	// the handle validator are all required. A partial s2sdk must disable the client features
	// instead of reporting "ready" with a controller handle of 0 (the world entity) and then
	// letting a deferred action compare that 0 against a real identity.
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_IsValidEntPointer == nullptr || __s2sdk_PlayerSlotToEntHandle == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientConnected(slot) || !s2sdk::IsClientInGame(slot) ||
			s2sdk::IsClientSourceTV(slot)) {
			return false;
		}
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr || !s2sdk::IsValidEntPointer(controller)) {
			return false;
		}
		// PITFALLS C1: 0 and -1 are the only invalid handle values; a negative handle with the
		// high serial bit set is valid.
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!IsValidEntityHandle(handle) || !s2sdk::IsValidEntHandle(handle)) {
			return false;
		}
		return true;
	} catch (...) {
		return false;
	}
}

bool IsBotSlot(int slot) {
	if (!HasController(slot)) {
		return true;
	}
	if (__s2sdk_IsFakeClient == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return true;
	}
}

int MaxClients() {
	if (__s2sdk_GetMaxClients == nullptr) {
		return kDefaultMaxClients;
	}
	try {
		const int value = s2sdk::GetMaxClients();
		return value > 0 && value <= kMaxSlots ? value : kDefaultMaxClients;
	} catch (...) {
		return kDefaultMaxClients;
	}
}

uint64_t ClientSteamID(int slot) {
	if (!HasController(slot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

// s2sdk answers "<unknown>" when it cannot resolve the client; the Pisex API returned an empty
// string there and the callers treat "" as "no name".
std::string ClientName(int slot) {
	if (!HasController(slot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		const std::string name = ToStd(s2sdk::GetClientName(slot));
		return name == "<unknown>" ? std::string() : name;
	} catch (...) {
		return {};
	}
}

int32_t ControllerHandleValue(int slot) {
	if (!HasController(slot)) {
		return -1; // -1 cannot collide with a valid handle; HasController already checked the pair
	}
	try {
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!IsValidEntityHandle(handle) || !s2sdk::IsValidEntHandle(handle)) {
			return -1;
		}
		return handle;
	} catch (...) {
		return -1;
	}
}

PlayerIdentity SlotIdentity(int slot) {
	PlayerIdentity identity;
	if (!HasController(slot)) {
		return identity;
	}
	const int32_t handle = ControllerHandleValue(slot);
	if (handle == -1) {
		return identity;
	}
	if (IsBotSlot(slot)) {
		// Bots have no SteamID; their controller handle is the identity (they never open menus or
		// receive membership actions, but the death handler uses the slot).
		identity.controllerHandle = handle;
		identity.valid = true;
		return identity;
	}
	const uint64_t steamid = ClientSteamID(slot);
	if (steamid == 0) {
		return identity;
	}
	identity.steamid = steamid;
	identity.controllerHandle = handle;
	identity.valid = true;
	return identity;
}

// The deferred work's precondition: the same human player is still on that slot. A different
// SteamID (or the same SteamID on a new controller entity after a reconnect) is a different
// player and must never inherit the action. Every check is unconditional: the identity must have
// a non-zero SteamID64 and a real (never 0 or -1) CEntityHandle, and the slot must still report
// exactly that handle.
bool PlayerValid(int slot, const PlayerIdentity& identity) {
	if (!g_mapReady || !ValidSlot(slot) || !identity.Valid()) {
		return false;
	}
	if (!HasController(slot) || IsBotSlot(slot)) {
		return false;
	}
	const int32_t controller = ControllerHandleValue(slot);
	if (!IsValidEntityHandle(controller) || controller != identity.controllerHandle) {
		return false;
	}
	const uint64_t steamid = ClientSteamID(slot);
	return steamid != 0 && steamid == identity.steamid;
}

// Membership bookkeeping is keyed by SteamID; this only re-checks that the SteamID still occupies
// that slot (a player reconnect of the same person is fine for a cache refresh).
bool SlotStillSteam(int slot, uint64_t steamid) {
	if (steamid == 0 || !g_mapReady || !ValidSlot(slot)) {
		return false;
	}
	if (!HasController(slot) || IsBotSlot(slot)) {
		return false;
	}
	return ClientSteamID(slot) == steamid;
}

uint64_t SlotSteam(int slot) {
	if (!HasController(slot) || IsBotSlot(slot)) {
		return 0;
	}
	return ClientSteamID(slot);
}

int FindSlotBySteam(uint64_t steamid) {
	if (steamid == 0 || !g_mapReady) {
		return -1;
	}
	const int maxClients = MaxClients();
	for (int slot = 0; slot < maxClients; ++slot) {
		if (!HasController(slot) || IsBotSlot(slot)) {
			continue;
		}
		if (ClientSteamID(slot) == steamid) {
			return slot;
		}
	}
	return -1;
}

// ---------------------------------------------------------------------------------------------
// Schema helpers (PITFALLS C3/C8: gate first, cache only a successful lookup)
// ---------------------------------------------------------------------------------------------

int ResolveOffset(int& cache, const char* className, const char* fieldName) {
	if (cache >= 0) {
		return cache;
	}
	if (!g_mapReady || __s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	int offset = -1;
	try {
		offset = s2sdk::GetSchemaOffset(ToPlg(className), ToPlg(fieldName));
	} catch (...) {
		offset = -1;
	}
	// Only a successful lookup is cached: before the schema system is initialised (no map yet)
	// every lookup fails, and a cached -1 would poison the field for the rest of the session.
	if (offset >= 0) {
		cache = offset;
	}
	return offset;
}

int ResolveChainOffset(const char* className) {
	if (!g_mapReady || __s2sdk_GetSchemaChainOffset == nullptr) {
		return -1;
	}
	try {
		const int offset = s2sdk::GetSchemaChainOffset(ToPlg(className));
		// s2sdk's SafeNetworkStateChanged() reads a chain offset >= 0 as "the object's chainer
		// sits at object + offset"; 0 would make it treat the vtable as a chainer. -1 is the
		// "the object itself is the entity" sentinel.
		return offset > 0 ? offset : -1;
	} catch (...) {
		return -1;
	}
}

bool HasPawn(int slot) {
	if (!HasController(slot) || __s2sdk_GetClientPawn == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetClientPawn(slot) != nullptr;
	} catch (...) {
		return false;
	}
}

// The team lives on the controller (CBaseEntity::m_iTeamNum, what Pisex read). s2sdk's
// GetClientTeam goes through the pawn and warns for every client without one, so the pawn export
// is only the fallback when the schema offset is unavailable.
int TeamOf(int slot) {
	if (!HasController(slot)) {
		return 0;
	}
	if (__s2sdk_GetSchemaOffset != nullptr && __s2sdk_GetEntData2 != nullptr &&
		__s2sdk_PlayerSlotToEntPointer != nullptr) {
		const int offset = ResolveOffset(g_teamOffset, "CBaseEntity", "m_iTeamNum");
		if (offset >= 0) {
			try {
				void* controller = s2sdk::PlayerSlotToEntPointer(slot);
				if (controller != nullptr) {
					return static_cast<int>(s2sdk::GetEntData2(controller, offset, 1) & 0xFF);
				}
			} catch (...) {
			}
		}
	}
	if (!HasPawn(slot) || __s2sdk_GetClientTeam == nullptr) {
		return 0;
	}
	try {
		return static_cast<int>(s2sdk::GetClientTeam(slot));
	} catch (...) {
		return 0;
	}
}

bool IsWarmup() {
	if (!g_mapReady || __s2sdk_GetGameRules == nullptr || __s2sdk_GetEntData2 == nullptr) {
		return false;
	}
	const int offset = ResolveOffset(g_warmupOffset, "CCSGameRules", "m_bWarmupPeriod");
	if (offset < 0) {
		return false;
	}
	try {
		void* rules = s2sdk::GetGameRules();
		if (rules == nullptr) {
			return false;
		}
		return s2sdk::GetEntData2(rules, offset, 1) != 0;
	} catch (...) {
		return false;
	}
}

// ---------------------------------------------------------------------------------------------
// Paths, config, phrases and database
// ---------------------------------------------------------------------------------------------

std::string ConfigsRoot() {
	return plg::GetConfigsDir != nullptr ? ToStd(plg::GetConfigsDir()) : std::string();
}

std::string DataRoot() {
	return plg::GetDataDir != nullptr ? ToStd(plg::GetDataDir()) : std::string();
}

std::string SdkServerLanguage() {
	if (__s2sdk_GetServerLanguage == nullptr) {
		return {};
	}
	try {
		return ToStd(s2sdk::GetServerLanguage());
	} catch (...) {
		return {};
	}
}

void LoadConfiguration() {
	// A reload with a missing/broken file falls back to the built-in defaults, exactly like the
	// first start (ParseConfig() only assigns on success).
	g_config = Config{};
	const std::string path = JoinPath(PluginConfigDir(ConfigsRoot(), kPluginName), "config.json");
	const std::optional<std::string> text = ReadTextFile(path);
	if (!text.has_value()) {
		LogError(std::string("[") + kLogTag + "] cannot read '" + path +
				 "' - the built-in defaults are used");
		g_invites.Configure(g_config.inviteExpireSeconds, g_config.inviteCooldownSeconds);
		return;
	}
	std::string error;
	if (!ParseConfig(*text, g_config, error)) {
		LogError(std::string("[") + kLogTag + "] " + error + " - the built-in defaults are used");
		g_invites.Configure(g_config.inviteExpireSeconds, g_config.inviteCooldownSeconds);
		return;
	}
	if (!error.empty()) {
		LogWarning(std::string("[") + kLogTag + "] " + error);
	}
	g_invites.Configure(g_config.inviteExpireSeconds, g_config.inviteCooldownSeconds);
}

void LoadPhrases() {
	const std::string configsDir = ConfigsRoot();
	// pisexcompat's DetectLanguage(): <configs>/core.cfg "ServerLang" first, then s2sdk's
	// ServerLanguage, then "en". Both are plain config reads, so this works at plugin start.
	const std::optional<std::string> coreConfig =
		configsDir.empty() ? std::nullopt : ReadTextFile(JoinPath(configsDir, "core.cfg"));
	g_language = DetectLanguage(coreConfig, SdkServerLanguage());
	g_phrases.clear();
	const std::string path =
		ResolveTranslationsPath(kPhrasesFile, configsDir, DataRoot(), kPluginName);
	if (!LoadPhrasesFile(path, g_language, g_phrases)) {
		LogError(std::string("[") + kLogTag + "] cannot read '" + path +
				 "' - phrase keys are printed as-is");
	}
}

void EnsureDataDirectory(const std::string& sqlitePath) {
	if (sqlitePath.empty() || sqlitePath == ":memory:") {
		return;
	}
	std::error_code error;
	const std::filesystem::path parent = std::filesystem::path(sqlitePath).parent_path();
	if (!parent.empty()) {
		std::filesystem::create_directories(parent, error);
		if (error) {
			LogError(std::string("[") + kLogTag + "] cannot create the data directory '" +
					 parent.string() + "': " + error.message());
		}
	}
}

void EnsureDatabase() {
	// The live clans database the website also uses. The connection is read from the plugin's own
	// databases.cfg section (PLUGIN_STANDARD §4); "sqlite" is selectable explicitly for tests and
	// offline development. When the file/section is missing the plugin fails soft onto one local
	// SQLite file (PLUGIN_STANDARD §4, fail soft) and writes a single error line.
	plgdb::Config database;
	database.driver = plgdb::Driver::MySQL;
	database.database = kPluginName;

	const std::string configsDir = ConfigsRoot();
	const std::string dataDir = DataRoot();
	const std::string path = ResolveConfigPath(kDatabasesCfg, configsDir, dataDir, kPluginName);

	bool haveConfig = false;
	bool unsupportedDriver = false;
	std::string configError;
	if (const std::optional<std::string> text = ReadTextFile(path); text.has_value()) {
		const DatabaseConfigResult parsed = ParseDatabaseConfig(*text, kPluginName);
		configError = parsed.error;
		if (parsed.found) {
			haveConfig = true;
			const DatabaseSettings& settings = parsed.settings;
			if (settings.driver == "sqlite") {
				database.driver = plgdb::Driver::SQLite;
			} else if (settings.driver == "mysql" || settings.driver == "mariadb") {
				database.driver = plgdb::Driver::MySQL;
			} else {
				LogError(std::string("[") + kLogTag + "] database driver '" + settings.driver +
						 "' is not supported (MySQL/MariaDB and SQLite only) - the runtime is "
						 "disabled");
				unsupportedDriver = true;
			}
			database.host = settings.host.empty() ? "127.0.0.1" : settings.host;
			database.port = static_cast<uint16_t>(
				std::max(1, std::min(65535, settings.port)));
			database.user = settings.user;
			database.password = settings.password;
			if (!settings.database.empty()) {
				database.database = settings.database;
			}
			if (database.driver == plgdb::Driver::SQLite) {
				const std::string file = settings.sqlitePath.empty() ? kSqliteFile : settings.sqlitePath;
				database.sqlitePath = ResolveDataPath(file, dataDir, kPluginName);
			}
		}
	}

	if (!haveConfig && !unsupportedDriver) {
		// Fail soft (PLUGIN_STANDARD §4): one local SQLite file keeps the plugin loaded and the
		// commands answering; the site's MySQL data is never forked because a missing section is
		// not a temporary outage (plgdb reconnects by itself for those). One error line.
		LogError(std::string("[") + kLogTag + "] " +
				 (configError.empty() ? std::string("no '") + kPluginName + "' entry in " + path
									  : path + ": " + configError) +
				 " - falling back to a local SQLite database");
		database.driver = plgdb::Driver::SQLite;
		database.sqlitePath = ResolveDataPath(kSqliteFile, dataDir, kPluginName);
	}
	if (unsupportedDriver) {
		return;
	}
	if (database.driver == plgdb::Driver::SQLite) {
		EnsureDataDirectory(database.sqlitePath);
	}

	g_database = plgdb::Database::Create(database);
	if (!g_database) {
		LogError(std::string("[") + kLogTag + "] the database configuration is invalid");
		return;
	}
	g_store = std::make_unique<GuildStore>(*g_database);
	// plgdb connects lazily on the worker; this says only that the config parsed.
	LogInfo(std::string("[") + kLogTag + "] database configured (" +
			(database.driver == plgdb::Driver::SQLite ? std::string("sqlite ") + database.sqlitePath
													  : std::string("mysql ") + database.database) +
			", connecting on first query)");
}

// ---------------------------------------------------------------------------------------------
// Chat and phrases
// ---------------------------------------------------------------------------------------------

std::string Phrase(std::string_view key) {
	return GetPhrase(g_phrases, key);
}

std::string FormatKey(std::string_view key, const std::vector<std::string>& args) {
	return FormatPhrase(Phrase(key), args);
}

std::string WithPrefix(const std::string& text) {
	return Phrase("k4.general.prefix") + " " + text;
}

void ReplyText(int slot, const std::string& text) {
	if (slot < 0) {
		// Pisex's PrintToConsole(-1) wrote to the server console; chat colour tags mean nothing
		// there, so the plugin log gets the stripped text.
		LogInfo(StripColorTags(text));
		return;
	}
	if (!g_mapReady || !HasController(slot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChat(slot, ToPlg(Colorize(text)));
	} catch (...) {
	}
}

void Reply(int slot, std::string_view key) {
	ReplyText(slot, WithPrefix(Phrase(key)));
}

void Reply(int slot, std::string_view key, const std::vector<std::string>& args) {
	ReplyText(slot, WithPrefix(FormatKey(key, args)));
}

void ReplyLines(int slot, const std::vector<std::string>& lines) {
	for (const std::string& line : lines) {
		ReplyText(slot, line);
	}
}

// One DB-backed interaction per second and per player slot; true when the caller must wait.
bool Throttle(std::chrono::steady_clock::time_point& last) {
	const auto now = std::chrono::steady_clock::now();
	if (last != std::chrono::steady_clock::time_point {} && now - last < std::chrono::seconds(1)) {
		return true;
	}
	last = now;
	return false;
}

bool CommandThrottled(int slot) {
	return slot >= 0 && slot < kMaxSlots && Throttle(g_lastCommandAt[static_cast<std::size_t>(slot)]);
}

bool MenuThrottled(int slot) {
	return slot >= 0 && slot < kMaxSlots && Throttle(g_lastMenuActionAt[static_cast<std::size_t>(slot)]);
}

std::string JoinArgs(const std::vector<std::string>& args, std::size_t from) {
	std::string joined;
	for (std::size_t index = from; index < args.size(); ++index) {
		if (!joined.empty()) {
			joined.push_back(' ');
		}
		joined += args[index];
	}
	return joined;
}

// Pisex handed the command callback CCommand::ArgS(), so a quoted token has to be quoted again:
// "mm_ban \"John Doe\" 60" reaches the handler as target "John" otherwise.
std::string QuoteToken(std::string_view token) {
	bool needsQuotes = token.empty();
	for (const char c : token) {
		if (std::isspace(static_cast<unsigned char>(c))) {
			needsQuotes = true;
			break;
		}
	}
	if (!needsQuotes || token.find('"') != std::string_view::npos) {
		return std::string(token);
	}
	std::string out;
	out.reserve(token.size() + 2);
	out.push_back('"');
	out.append(token);
	out.push_back('"');
	return out;
}

std::string BuildCommandContent(const plg::vector<plg::string>& args) {
	if (args.empty()) {
		return {};
	}
	std::string out = ToStd(args[0]);
	for (std::size_t index = 1; index < args.size(); ++index) {
		out.push_back(' ');
		out += QuoteToken(ToStd(args[index]));
	}
	return out;
}

// ---------------------------------------------------------------------------------------------
// Menu state (PITFALLS C10)
// ---------------------------------------------------------------------------------------------

struct MenuEntry {
	std::string info;
	std::string text;
	bool disabled = false;
};

struct MenuSlot {
	s2sdk::MenuId id = 0;
	bool valid = false;
	PlayerIdentity identity;
	std::vector<MenuEntry> entries;
};

MenuSlot g_menus[kMaxSlots];
std::deque<s2sdk::MenuId> g_menuDestroy;
std::vector<s2sdk::MenuId> g_parkedMenus;
s2sdk::MenuId g_reservedMenuId = 0;
bool g_hasReservedMenu = false;
bool g_menuUnavailableLogged = false;

// The say/say_team listener and the command registrations this plugin has to take back on unload.
struct CommandRegistration {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	bool attempted = false;
	bool owned = false;
};

struct ListenerRegistration {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	bool registered = false;
};

std::vector<CommandRegistration> g_commands;
std::vector<ListenerRegistration> g_listeners;
std::vector<std::string> g_hookedEvents;

// A selection queued by the menu handler: it is applied from OnPluginUpdate, outside the menu
// manager's lock, and only for the identity that was displayed.
struct PendingAction {
	int slot = -1;
	PlayerIdentity identity;
	std::function<void()> fn;
};

std::deque<PendingAction> g_pendingActions;

// ---------------------------------------------------------------------------------------------
// Registration helpers (A1/A7)
// ---------------------------------------------------------------------------------------------

bool MenuTypeRegistered(const char* name) {
	if (__s2sdk_IsMenuTypeRegistered == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsMenuTypeRegistered(ToPlg(name));
	} catch (...) {
		return false;
	}
}

// PITFALLS C10: all eight menu bindings are required together, before any CreateMenu call. Without
// a teardown binding a created menu could not be released safely (A7).
bool MenuBindingsReady() {
	return __s2sdk_CreateMenu != nullptr && __s2sdk_DestroyMenu != nullptr &&
		   __s2sdk_DisplayMenu != nullptr && __s2sdk_AddMenuItem != nullptr &&
		   __s2sdk_GetClientMenu != nullptr && __s2sdk_CancelClientMenu != nullptr &&
		   __s2sdk_SetMenuType != nullptr && __s2sdk_GetMenuType != nullptr;
}

void LogMenuBindingsUnavailableOnce() {
	if (g_menuUnavailableLogged) {
		return;
	}
	g_menuUnavailableLogged = true;
	LogError(std::string("[") + kLogTag +
			 "] the s2sdk menu bindings are unavailable (Create/Destroy/Display/AddItem/"
			 "GetClientMenu/CancelClientMenu/SetMenuType/GetMenuType are required) - the menu is "
			 "disabled");
}

// The player's own "!menu" choice from the optional hudmenu plugin. Without hudmenu - or with an
// unknown/stale answer - pisexcompat's HUD_LAYOUT default applies (see k4::ResolveMenuTypeName).
std::string ResolveMenuType(int slot) {
	std::string choice;
	if (__hudmenu_GetClientMenuType != nullptr) {
		try {
			choice = ToStd(hudmenu::GetClientMenuType(slot));
		} catch (...) {
			choice.clear();
		}
	}
	return ResolveMenuTypeName(choice, MenuTypeRegistered("hud"), MenuTypeRegistered("wasd"));
}

// s2sdk menu id 0 is a real id (the first CreateMenu returns it) *and* the manager's "no menu"
// sentinel (client state 0), so a dummy menu burns it at start. Every real menu then gets a
// non-zero id. The reservation only happens when every teardown binding is present.
bool ReserveMenuIdZero() {
	if (g_hasReservedMenu) {
		return true;
	}
	if (!MenuBindingsReady()) {
		return false;
	}
	try {
		g_reservedMenuId = s2sdk::CreateMenu(plg::string("k4_guilds:reserved-id-0"), nullptr,
											 ToPlg("chat"));
		g_hasReservedMenu = true;
		if (g_reservedMenuId != 0) {
			// Another plugin already burned id 0: the dummy is not needed.
			if (__s2sdk_DestroyMenu != nullptr) {
				try {
					s2sdk::DestroyMenu(g_reservedMenuId);
				} catch (...) {
				}
			}
			g_hasReservedMenu = false;
			g_reservedMenuId = 0;
		}
		return true;
	} catch (...) {
		g_hasReservedMenu = false;
		g_reservedMenuId = 0;
		return false;
	}
}

int FindDisplaySlot(s2sdk::MenuId id) {
	if (id == 0 || __s2sdk_GetClientMenu == nullptr) {
		return -1;
	}
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (s2sdk::GetClientMenu(slot) == id) {
				return slot;
			}
		} catch (...) {
		}
	}
	return -1;
}

// Switches a menu to a builtin whose close() is a no-op ("chat"/"console"), so a DestroyMenu that
// still has to end a display cannot reach the client layer. Verified through GetMenuType.
bool MakeMenuCloseNoOp(s2sdk::MenuId id) {
	if (__s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr) {
		return false;
	}
	for (const char* type : {"chat", "console"}) {
		if (__s2sdk_IsMenuTypeRegistered != nullptr && !MenuTypeRegistered(type)) {
			continue;
		}
		try {
			if (!s2sdk::SetMenuType(id, ToPlg(type))) {
				continue;
			}
			if (ToStd(s2sdk::GetMenuType(id)) == type) {
				return true;
			}
		} catch (...) {
		}
	}
	return false;
}

void ParkMenu(s2sdk::MenuId id) {
	if (id == 0) {
		return;
	}
	for (const s2sdk::MenuId queued : g_parkedMenus) {
		if (queued == id) {
			return;
		}
	}
	if (g_parkedMenus.size() >= 64) {
		LogError(std::string("[") + kLogTag + "] A7_UNRESOLVED: too many parked menus, cannot track menu id " +
				 std::to_string(id));
		return;
	}
	g_parkedMenus.push_back(id);
}

bool DestroyMenuDirect(s2sdk::MenuId id) {
	if (__s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	bool destroyed = false;
	try {
		destroyed = s2sdk::DestroyMenu(id);
	} catch (...) {
		return false;
	}
	if (__s2sdk_IsValidMenu != nullptr) {
		try {
			return !s2sdk::IsValidMenu(id);
		} catch (...) {
		}
	}
	return destroyed;
}

// Ends every display of `id` and destroys the menu; the caller must not be inside a menu handler.
// mapReady == false is the changelevel window, where a backend close must not touch a client.
bool DestroyMenuChecked(s2sdk::MenuId id, bool /*mapReady*/) {
	if (__s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	if (id == 0) {
		// The reserved dummy id: the manager's client state 0 means "no menu", so it cannot be
		// displayed and the plain destroy is safe even between maps.
		return DestroyMenuDirect(id);
	}
	if (const int displaySlot = FindDisplaySlot(id); displaySlot >= 0) {
		if (__s2sdk_CancelClientMenu != nullptr) {
			try {
				s2sdk::CancelClientMenu(displaySlot, s2sdk::MenuCancelReason::Disconnect);
			} catch (...) {
			}
		}
		// Re-query the manager even when CancelClientMenu answered true: a display that survived
		// the cancel must not reach a custom backend's close (on a live map it is safe, between
		// maps it touches the torn-down client layer). The builtin no-op-close type switch is the
		// only safe path in both cases.
		if (FindDisplaySlot(id) >= 0 && !MakeMenuCloseNoOp(id)) {
			return false;
		}
	}
	return DestroyMenuDirect(id);
}

std::vector<s2sdk::MenuId> CollectOwnedMenuIds() {
	std::vector<s2sdk::MenuId> ids;
	for (MenuSlot& menu : g_menus) {
		if (menu.valid && menu.id != 0) {
			ids.push_back(menu.id);
		}
		menu = MenuSlot{};
	}
	for (const s2sdk::MenuId id : g_menuDestroy) {
		if (id != 0) {
			ids.push_back(id);
		}
	}
	g_menuDestroy.clear();
	for (const s2sdk::MenuId id : g_parkedMenus) {
		if (id != 0) {
			ids.push_back(id);
		}
	}
	g_parkedMenus.clear();
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	return ids;
}

void TeardownMenusOnMapEnd() {
	g_pendingActions.clear();
	// OnMapEnd runs before the client layer is torn down: only state changes here (PITFALLS B2).
	for (const s2sdk::MenuId id : CollectOwnedMenuIds()) {
		if (DestroyMenuChecked(id, /*mapReady=*/false)) {
			continue;
		}
		ParkMenu(id);
		LogWarning(std::string("[") + kLogTag + "] A7_UNRESOLVED (parked): could not close menu " +
				   std::to_string(id) + " in the changelevel window - retried when the next map is up");
	}
}

void RetryParkedMenus() {
	if (g_parkedMenus.empty() || !g_mapReady) {
		return;
	}
	std::vector<s2sdk::MenuId> ids = std::move(g_parkedMenus);
	g_parkedMenus.clear();
	for (const s2sdk::MenuId id : ids) {
		if (DestroyMenuChecked(id, /*mapReady=*/true)) {
			continue;
		}
		ParkMenu(id);
		LogError(std::string("[") + kLogTag + "] A7_UNRESOLVED (parked): could not destroy menu " +
				 std::to_string(id) + " with a live map - keeping it for unload");
	}
}

void TeardownMenusOnUnload() {
	g_pendingActions.clear();
	const bool mapReady = g_mapReady;
	std::vector<s2sdk::MenuId> ids = CollectOwnedMenuIds();
	if (g_hasReservedMenu) {
		ids.push_back(g_reservedMenuId);
		g_hasReservedMenu = false;
		g_reservedMenuId = 0;
	}
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	std::vector<s2sdk::MenuId> unresolved;
	for (const s2sdk::MenuId id : ids) {
		if (!DestroyMenuChecked(id, mapReady)) {
			unresolved.push_back(id);
		}
	}
	g_menuDestroy.clear();
	g_parkedMenus.clear();
	if (!unresolved.empty()) {
		std::string list;
		for (const s2sdk::MenuId id : unresolved) {
			if (!list.empty()) {
				list += ", ";
			}
			list += std::to_string(id);
		}
		LogError(std::string("[") + kLogTag + "] A7_UNRESOLVED: s2sdk refused to release menu id(s) " +
				 list +
				 " - their handler points into this library and cannot be removed by the plugin API");
	}
}

void QueueMenuDestroy(s2sdk::MenuId id) {
	if (id == 0 || g_menuDestroy.size() >= kMaxQueuedMenuDestroys) {
		return;
	}
	g_menuDestroy.push_back(id);
}

void DrainMenuDestroys() {
	// Between maps the queue is left for OnMapEnd/OnPluginEnd: CollectOwnedMenuIds() takes it
	// together with the live menus. Dropping entries here would leak their handlers.
	if (!g_mapReady) {
		return;
	}
	while (!g_menuDestroy.empty()) {
		const s2sdk::MenuId id = g_menuDestroy.front();
		g_menuDestroy.pop_front();
		if (__s2sdk_IsValidMenu != nullptr) {
			try {
				if (!s2sdk::IsValidMenu(id)) {
					continue;
				}
			} catch (...) {
				continue;
			}
		}
		if (DestroyMenuDirect(id)) {
			continue;
		}
		ParkMenu(id);
		LogError(std::string("[") + kLogTag + "] could not destroy menu " + std::to_string(id) +
				 " - parked for retry");
	}
}

// ---------------------------------------------------------------------------------------------
// Deferred menu actions
// ---------------------------------------------------------------------------------------------

void QueueAction(int slot, const PlayerIdentity& identity, std::function<void()> fn) {
	if (!fn || !identity.Valid() || g_pendingActions.size() >= kMaxQueuedActions) {
		return;
	}
	g_pendingActions.push_back(PendingAction{slot, identity, std::move(fn)});
}

bool CanRunAction(const PendingAction& action) {
	// One implementation of the identity rule for both the queued-action drain and every DB
	// callback: no second, weaker comparison can drift from it.
	return PlayerValid(action.slot, action.identity);
}

void DrainActions();

// Runs under the menu manager's lock: it may only read menu state and queue work. No client,
// entity, schema or display call happens here (PITFALLS C10).
void OnMenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param) {
	try {
		if (!ValidSlot(slot)) {
			return;
		}
		MenuSlot& menu = g_menus[slot];
		if (!menu.valid || menu.id != id) {
			return; // stale (the plugin already closed/replaced it)
		}
		switch (action) {
		case s2sdk::MenuAction::Start:
			return;
		case s2sdk::MenuAction::Select: {
			// s2sdk accepts digits 1..7 even when the page shows five items and maps them to
			// currentOffset + digit - 1; Pisex numbers the five visible items 1..5. Derive the
			// visible item from the page offset and ignore selections outside the current page.
			int offset = param - param % kMenuItemsPerPage;
			if (__s2sdk_GetClientMenuOffset != nullptr) {
				try {
					offset = s2sdk::GetClientMenuOffset(slot);
				} catch (...) {
				}
			}
			if (param < offset || param >= offset + kMenuItemsPerPage) {
				return;
			}
			if (param < 0 || static_cast<std::size_t>(param) >= menu.entries.size()) {
				return;
			}
			const MenuEntry entry = menu.entries[static_cast<std::size_t>(param)];
			if (entry.disabled || entry.info.empty()) {
				return;
			}
			const PlayerIdentity identity = menu.identity;
			QueueAction(slot, identity,
						[slot, info = entry.info] { HandleMenuAction(slot, info); });
			return; // closeOnSelect is false: the menu stays until the action replaces it
		}
		case s2sdk::MenuAction::Cancel:
		case s2sdk::MenuAction::End:
			// The display session is over; the id must be destroyed by the plugin (s2sdk never
			// destroys a closed menu). No s2sdk call may happen here - the handler runs under the
			// menu manager's lock.
			menu = MenuSlot{};
			QueueMenuDestroy(id);
			return;
		}
	} catch (const std::exception& exception) {
		LogError(std::string("[") + kLogTag + "] menu handler failed: " + exception.what());
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] menu handler failed with an unknown exception");
	}
}

// Replaces a slot's menu. Safe destruction first; when that fails the new menu is not created, so
// two displays/handlers are never mixed.
bool ReleaseSlotMenu(int slot, bool mapReady) {
	MenuSlot& menu = g_menus[slot];
	if (!menu.valid) {
		return true;
	}
	const s2sdk::MenuId id = menu.id;
	menu = MenuSlot{};
	bool released = false;
	if (mapReady) {
		// With a live map a backend close is safe: the menu's own display is ended by DestroyMenu.
		released = DestroyMenuDirect(id);
	} else {
		released = DestroyMenuChecked(id, /*mapReady=*/false);
	}
	if (released) {
		return true;
	}
	ParkMenu(id);
	LogError(std::string("[") + kLogTag + "] could not release menu " + std::to_string(id) +
			 " on slot " + std::to_string(slot) + " - not opening a replacement");
	return false;
}

void DisplayBuiltMenu(int slot, const PlayerIdentity& identity, const std::string& rawTitle,
					  std::vector<MenuEntry> entries, bool exitBack) {
	if (!g_mapReady || !PlayerValid(slot, identity)) {
		return;
	}
	if (!MenuBindingsReady()) {
		LogMenuBindingsUnavailableOnce();
		return;
	}
	if (!ReserveMenuIdZero()) {
		LogError(std::string("[") + kLogTag +
				 "] could not reserve s2sdk menu id 0 - the clan menu is disabled");
		return;
	}
	if (!ReleaseSlotMenu(slot, /*mapReady=*/true)) {
		return;
	}
	// Tracked outside the try: an exception between CreateMenu and the registry write must not
	// leave a menu (and its handler pointer into this library) behind.
	s2sdk::MenuId createdId = 0;
	try {
		const s2sdk::MenuId id = s2sdk::CreateMenu(
			ToPlg(StripColorTags(rawTitle)), &OnMenuHandler, ToPlg(ResolveMenuType(slot)));
		createdId = id;
		if (id == 0) {
			// Only the dummy this plugin reserved may be destroyed here; an id 0 owned by another
			// plugin is never touched.
			if (g_hasReservedMenu && g_reservedMenuId == 0) {
				(void)DestroyMenuDirect(0);
			}
			g_hasReservedMenu = false;
			g_reservedMenuId = 0;
			LogError(std::string("[") + kLogTag +
					 "] s2sdk returned menu id 0 - the menu is not displayed");
			return;
		}
		MenuSlot& menu = g_menus[slot];
		for (const MenuEntry& entry : entries) {
			const s2sdk::MenuItemStyle style =
				entry.disabled ? s2sdk::MenuItemStyle::Disabled : s2sdk::MenuItemStyle::Default;
			if (s2sdk::AddMenuItem(id, ToPlg(entry.info), ToPlg(StripColorTags(entry.text)), style)
				< 0) {
				LogError(std::string("[") + kLogTag + "] could not add a menu item");
				if (!DestroyMenuDirect(id)) {
					ParkMenu(id);
				}
				return;
			}
		}
		if (__s2sdk_SetMenuPagination != nullptr) {
			s2sdk::SetMenuPagination(id, kMenuItemsPerPage); // chat pages of 5, like cs2-menus
		}
		if (__s2sdk_SetMenuExitButton != nullptr) {
			s2sdk::SetMenuExitButton(id, true);
		}
		if (__s2sdk_SetMenuExitBackButton != nullptr) {
			s2sdk::SetMenuExitBackButton(id, exitBack);
		}
		if (__s2sdk_SetMenuCloseOnSelect != nullptr) {
			// Pisex keeps the menu open after a selection; the callback decides what happens next.
			s2sdk::SetMenuCloseOnSelect(id, false);
		}
		menu.id = id;
		menu.valid = true;
		menu.identity = identity;
		menu.entries = std::move(entries);
		bool displayed = false;
		try {
			displayed = s2sdk::DisplayMenu(id, slot, kMenuTimeoutSeconds);
		} catch (...) {
			displayed = false;
		}
		if (displayed) {
			return;
		}
		menu = MenuSlot{};
		if (!DestroyMenuDirect(id)) {
			ParkMenu(id);
		}
		LogError(std::string("[") + kLogTag + "] could not display the clan menu on slot " +
				 std::to_string(slot));
	} catch (const std::exception& exception) {
		g_menus[slot] = MenuSlot{};
		if (createdId != 0 && !DestroyMenuDirect(createdId)) {
			ParkMenu(createdId);
		}
		LogError(std::string("[") + kLogTag + "] could not open the clan menu: " + exception.what());
	} catch (...) {
		g_menus[slot] = MenuSlot{};
		if (createdId != 0 && !DestroyMenuDirect(createdId)) {
			ParkMenu(createdId);
		}
		LogError(std::string("[") + kLogTag + "] could not open the clan menu");
	}
}

// ---------------------------------------------------------------------------------------------
// Player cache and reconcile
// ---------------------------------------------------------------------------------------------

PlayerInfo* FindBySteam(uint64_t steamid) {
	const auto entry = g_players.find(steamid);
	return entry == g_players.end() ? nullptr : &entry->second;
}

bool DatabaseBacklogged() {
	return g_database != nullptr && g_database->Pending() >= kMaxPendingStatements;
}

void LogLoadError(const std::string& error) {
	const auto now = std::chrono::steady_clock::now();
	if (g_lastLoadErrorLog != std::chrono::steady_clock::time_point {}
		&& now - g_lastLoadErrorLog < std::chrono::seconds(30)) {
		return; // one line per interval: an outage fails every player on every refresh pass
	}
	g_lastLoadErrorLog = now;
	LogWarning(std::string("[") + kLogTag + "] membership load failed: " + error);
}

void OnPlayerLoadFailed(uint64_t steamid, uint64_t generation, const std::string& error) {
	const auto entry = g_players.find(steamid);
	if (entry == g_players.end() || entry->second.generation != generation) {
		return;
	}
	// A failed query is not a kick: keep the last good clanId/tag (and the scoreboard tag) and
	// let the next refresh retry. loaded stays false for a player who never loaded, so the
	// death-reward path keeps using its LoadClanId fallback instead of believing clanId 0.
	entry->second.loading = false;
	LogLoadError(error);
}

void ApplyScoreboardTag(int slot, const std::string& tag);

void OnPlayerLoaded(uint64_t steamid, uint64_t generation, std::unique_ptr<Guild> guild,
					const std::string& error) {
	if (!error.empty()) {
		OnPlayerLoadFailed(steamid, generation, error);
		return;
	}
	const auto entry = g_players.find(steamid);
	if (entry == g_players.end() || entry->second.generation != generation) {
		return; // the player left or a newer load replaced this one
	}
	PlayerInfo& info = entry->second;
	const bool hadGuild = info.clanId > 0 || !info.clanTag.empty();
	info.loading = false;
	info.loaded = true;
	if (guild) {
		info.clanId = guild->id;
		info.clanTag = guild->tag;
	} else {
		info.clanId = 0;
		info.clanTag.clear();
	}
	if (!SlotStillSteam(info.slot, steamid)) {
		return;
	}
	if (guild) {
		if (g_config.showTagOnScoreboard) {
			ApplyScoreboardTag(info.slot, FormatClanTag(guild->tag));
		}
	} else if (hadGuild && g_config.showTagOnScoreboard) {
		// The site may remove the membership at any time (kick/leave/disband on the website):
		// the refresh notices the clan is gone and clears the stale scoreboard tag exactly once.
		ApplyScoreboardTag(info.slot, std::string());
	}
	if (g_store && guild && FindMember(*guild, info.auth) != nullptr) {
		// An unresolved name ("<unknown>") must not overwrite the site's stored name with an
		// empty string; the next refresh retries.
		const std::string name = ClientName(info.slot);
		if (!name.empty()) {
			g_store->TouchMember(info.auth, name);
		}
	}
}

void FinishReconcile(const std::shared_ptr<ReconcilePass>& pass) {
	if (pass == nullptr || pass->id != g_activeReconcilePass) {
		// An abandoned pass, or one that a newer pass already replaced, must not clear the
		// in-flight flag of the current pass.
		return;
	}
	g_reconcileInFlight = false;
	g_activeReconcilePass = 0;
}

void LoadPassGuilds(const std::shared_ptr<ReconcilePass>& pass);

void ReconcileAll(bool force);

PlayerInfo& BeginPlayerLoad(int slot, uint64_t steamid) {
	if (slot >= 0 && slot < kMaxSlots) {
		g_slotSteam[static_cast<std::size_t>(slot)] = steamid;
	}
	PlayerInfo& info = g_players[steamid];
	info.steamid = steamid;
	info.slot = slot;
	info.auth = std::to_string(steamid);
	info.generation = ++g_nextGeneration;
	info.loading = true;
	return info;
}

void LoadPassGuilds(const std::shared_ptr<ReconcilePass>& pass) {
	if (pass->clans.empty()) {
		FinishReconcile(pass);
		return;
	}
	pass->awaitingGuilds = pass->clans.size();
	for (auto& entry : pass->clans) {
		const int32_t clanId = entry.first;
		const std::vector<ReconcileLoad> members = entry.second;
		g_store->LoadGuildChecked(
			clanId, [pass, members](std::unique_ptr<Guild> guild, const std::string& error) {
				try {
					for (const ReconcileLoad& member : members) {
						if (!error.empty()) {
							OnPlayerLoadFailed(member.steamid, member.generation, error);
						} else if (guild) {
							// OnPlayerLoaded takes ownership and only keeps id/tag: hand each
							// member its own copy of the shared snapshot.
							OnPlayerLoaded(member.steamid, member.generation,
										   std::make_unique<Guild>(*guild), std::string());
						} else {
							OnPlayerLoaded(member.steamid, member.generation, nullptr,
										   std::string());
						}
					}
				} catch (...) {
				}
				if (pass->awaitingGuilds > 0) {
					--pass->awaitingGuilds;
				}
				if (pass->awaitingGuilds == 0) {
					FinishReconcile(pass);
				}
			});
	}
}

void ReconcileAll(bool force) {
	if (!g_store || !g_mapReady) {
		return;
	}
	// Backpressure: a slow or unreachable database must not accumulate reconcile statements.
	if (DatabaseBacklogged()) {
		return;
	}
	if (g_reconcileInFlight) {
		// A pass whose store-side callbacks never arrived (an exception while parsing rows,
		// caught and logged by plgdb) must not disable the refresh for the rest of the session.
		const bool lost = g_database != nullptr && g_database->Pending() == 0
						  && std::chrono::steady_clock::now() - g_reconcileStartedAt
								 >= std::chrono::seconds(30);
		if (!lost) {
			return;
		}
		LogWarning(std::string("[") + kLogTag +
				   "] a membership refresh pass lost its callbacks; starting a new one");
		g_reconcileInFlight = false;
		g_activeReconcilePass = 0;
	}
	std::vector<ReconcileLoad> loads;
	const int maxClients = MaxClients();
	for (int slot = 0; slot < maxClients; ++slot) {
		if (!HasController(slot) || IsBotSlot(slot)) {
			continue;
		}
		const uint64_t steamid = ClientSteamID(slot);
		if (steamid == 0) {
			continue;
		}
		const auto existing = g_players.find(steamid);
		if (existing != g_players.end()) {
			existing->second.slot = slot;
			g_slotSteam[static_cast<std::size_t>(slot)] = steamid;
			if (!force && (existing->second.loading || existing->second.loaded)) {
				continue;
			}
		}
		const PlayerInfo& info = BeginPlayerLoad(slot, steamid);
		loads.push_back(ReconcileLoad{steamid, info.generation, info.auth});
	}
	// Drop cache entries for players who left without a player_disconnect (reload, bots).
	for (auto it = g_players.begin(); it != g_players.end();) {
		if (FindSlotBySteam(it->first) < 0) {
			it = g_players.erase(it);
		} else {
			++it;
		}
	}
	if (loads.empty()) {
		return;
	}
	// One LoadClanId per player, then one full guild load per distinct clan: the older path ran
	// five queries per player and re-read a whole clan roster once per online member.
	auto pass = std::make_shared<ReconcilePass>();
	pass->id = ++g_nextReconcilePass;
	pass->awaitingClanIds = loads.size();
	g_activeReconcilePass = pass->id;
	g_reconcileStartedAt = std::chrono::steady_clock::now();
	g_reconcileInFlight = true;
	try {
		for (const ReconcileLoad& load : loads) {
			g_store->LoadClanIdChecked(
				load.auth, [pass, steamid = load.steamid, generation = load.generation](
							   int32_t clanId, const std::string& error) {
					// The accounting must run even if one callback fails: a stuck pass would
					// disable the periodic refresh for the rest of the session.
					try {
						if (!error.empty()) {
							OnPlayerLoadFailed(steamid, generation, error);
						} else if (clanId <= 0) {
							OnPlayerLoaded(steamid, generation, nullptr, std::string());
						} else {
							pass->clans[clanId].push_back(
								ReconcileLoad{steamid, generation, std::string()});
						}
					} catch (...) {
					}
					if (pass->awaitingClanIds > 0) {
						--pass->awaitingClanIds;
					}
					if (pass->awaitingClanIds == 0) {
						if (pass->id != g_activeReconcilePass) {
							return; // a self-heal already replaced this pass
						}
						try {
							LoadPassGuilds(pass);
						} catch (...) {
							FinishReconcile(pass);
						}
					}
				});
		}
	} catch (...) {
		// A failed pass start must not leave the refresh disabled forever; the next periodic
		// call starts a new pass.
		FinishReconcile(pass);
	}
}

void EnsurePlayer(int slot, uint64_t steamid, bool force) {
	if (!g_store || steamid == 0) {
		return;
	}
	if (slot >= 0 && slot < kMaxSlots) {
		g_slotSteam[static_cast<std::size_t>(slot)] = steamid;
	}
	const auto existing = g_players.find(steamid);
	if (existing != g_players.end()) {
		existing->second.slot = slot;
		if (!force && (existing->second.loading || existing->second.loaded)) {
			return;
		}
	}
	const PlayerInfo& info = BeginPlayerLoad(slot, steamid);
	const uint64_t generation = info.generation;
	g_store->LoadGuildForPlayerChecked(
		info.auth, [steamid, generation](std::unique_ptr<Guild> guild, const std::string& error) {
			OnPlayerLoaded(steamid, generation, std::move(guild), error);
		});
}

// ---------------------------------------------------------------------------------------------
// Guild helpers and actions
// ---------------------------------------------------------------------------------------------

using GuildAction = std::function<void(uint64_t steamid, std::unique_ptr<Guild>)>;

void WithGuild(int slot, GuildAction action) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !action || !PlayerValid(slot, identity)) {
		return;
	}
	if (DatabaseBacklogged()) {
		Reply(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const uint64_t steamid = identity.steamid;
	const std::string auth = std::to_string(steamid);
	g_store->LoadGuildForPlayerChecked(
		auth, [slot, identity, action = std::move(action)](std::unique_ptr<Guild> guild,
														  const std::string& error) mutable {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!error.empty()) {
				// A failed query is not "not in a guild": keep the cached membership.
				LogLoadError(error);
				Reply(slot, "k4.menu.error.db_unavailable");
				return;
			}
			if (!guild) {
				Reply(slot, "k4.guild.error.not_in_guild");
				return;
			}
			PlayerInfo& info = g_players[identity.steamid];
			info.steamid = identity.steamid;
			info.slot = slot;
			info.auth = std::to_string(identity.steamid);
			info.clanId = guild->id;
			info.clanTag = guild->tag;
			info.loaded = true;
			action(identity.steamid, std::move(guild));
		});
}

void ApplyScoreboardTag(int slot, const std::string& tag) {
	if (!g_config.showTagOnScoreboard || !g_mapReady) {
		return;
	}
	if (IsBotSlot(slot)) {
		return; // bots have no SteamID and never carry a clan tag (PITFALLS C2)
	}
	// PITFALLS C1: only 0 and -1 are invalid handle values; a negative handle is valid.
	const int32_t handle = ControllerHandleValue(slot);
	if (handle == -1) {
		return;
	}
	// m_szClan is a CUtlSymbolLarge (a pooled-string pointer), not a CUtlString. The typed
	// SetEntString/SetEntSchemaString would run CUtlString::operator= over that slot: for a player
	// whose m_szClan already holds a Steam-group symbol it frees the symbol-table pointer as if it
	// were its own heap allocation, and the replacement allocation leaks into the entity field.
	// The raw handle writer with size -1 allocates a pooled string and stores the symbol exactly
	// like the engine does (PITFALLS C3/C8: handle first, gate open, cache only a success).
	if (g_clanOffset < 0) {
		if (__s2sdk_GetSchemaOffset == nullptr) {
			return;
		}
		const int offset = ResolveOffset(g_clanOffset, "CCSPlayerController", "m_szClan");
		if (offset < 0) {
			return; // schema not ready yet: retry on the next refresh
		}
		if (g_clanChainOffset < 0) {
			g_clanChainOffset = ResolveChainOffset("CCSPlayerController");
		}
	}
	if (__s2sdk_SetEntDataCString == nullptr) {
		return;
	}
	try {
		s2sdk::SetEntDataCString(handle, g_clanOffset, ToPlg(tag), -1, true, g_clanChainOffset);
	} catch (...) {
	}
}

std::string FormatClanTag(const std::string& tag) {
	// The CS2 scoreboard wraps m_szClan in brackets itself: FormatClanTagText strips one stored
	// wrapper ("[ADM]" -> "ADM") and applies ClanTag_Format (default "{1}").
	return FormatClanTagText(Phrase("ClanTag_Format"), tag);
}

std::string FormatTime(int64_t timestamp) {
	if (timestamp <= 0) {
		return "-";
	}
	const std::time_t value = static_cast<std::time_t>(timestamp);
	std::tm tm {};
	localtime_r(&value, &tm);
	char buffer[32] = {};
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
	return buffer;
}

void LogAction(int32_t clanId, const char* action, const std::string& value) {
	if (!g_store || !ShouldLogAction(g_config, action)) {
		return;
	}
	g_store->WriteLog(clanId, action, value);
}

void ClearMemberCacheAndTag(uint64_t steamid) {
	if (steamid == 0) {
		return;
	}
	const int slot = FindSlotBySteam(steamid);
	if (slot >= 0) {
		ApplyScoreboardTag(slot, "");
	}
	const auto entry = g_players.find(steamid);
	if (entry != g_players.end()) {
		entry->second.clanId = 0;
		entry->second.clanTag.clear();
	}
	if (slot >= 0) {
		EnsurePlayer(slot, steamid, true);
	}
}

// ---------------------------------------------------------------------------------------------
// Commands (PITFALLS C11, A7)
// ---------------------------------------------------------------------------------------------

bool OnMainCommand(int slot, const char* content);
bool OnGuildChatCommand(int slot, const char* content);

s2sdk::ResultType OnMainCommandCallback(int32_t caller, s2sdk::ConCommandContext context,
										const plg::vector<plg::string>& args) {
	try {
		if (g_shuttingDown || args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		// The engine delivers the server console as caller -1 and a player console/chat as a real
		// slot. Anything else is not a source this command answers for.
		if (caller != -1 && !ValidSlot(caller)) {
			return s2sdk::ResultType::Handled;
		}
		// Chat context always has a player caller; a malformed one is swallowed, never run as
		// server console.
		if (context == s2sdk::ConCommandContext::Chat && !ValidSlot(caller)) {
			return s2sdk::ResultType::Handled;
		}
		OnMainCommand(caller, BuildCommandContent(args).c_str());
	} catch (const std::exception& exception) {
		LogError(std::string("[") + kLogTag + "] command failed: " + exception.what());
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] command failed with an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnGuildChatCommandCallback(int32_t caller, s2sdk::ConCommandContext context,
											 const plg::vector<plg::string>& args) {
	try {
		if (g_shuttingDown || args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		if (caller != -1 && !ValidSlot(caller)) {
			return s2sdk::ResultType::Handled;
		}
		if (context == s2sdk::ConCommandContext::Chat && !ValidSlot(caller)) {
			return s2sdk::ResultType::Handled;
		}
		OnGuildChatCommand(caller, BuildCommandContent(args).c_str());
	} catch (const std::exception& exception) {
		LogError(std::string("[") + kLogTag + "] guild chat failed: " + exception.what());
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] guild chat failed with an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

// The "." aliases: s2sdk only dispatches its configured "!" / "/" triggers, so a say/say_team
// pre-listener catches the old Metamod ".clan" spelling, like the previous port did.
s2sdk::ResultType OnSayListener(int32_t caller, s2sdk::ConCommandContext /*context*/,
								const plg::vector<plg::string>& args) {
	try {
		if (g_shuttingDown || !g_mapReady || !ValidSlot(caller) || args.size() < 2) {
			return s2sdk::ResultType::Continue;
		}
		std::string content = ToStd(args[1]);
		for (std::size_t index = 2; index < args.size(); ++index) {
			content.push_back(' ');
			content += ToStd(args[index]);
		}
		const std::vector<std::string> tokens = TokenizeCommand(content);
		if (tokens.empty() || tokens.front().size() < 2 || tokens.front().front() != '.') {
			return s2sdk::ResultType::Continue;
		}
		const bool matches = std::any_of(
			g_config.chatAliases.begin(), g_config.chatAliases.end(),
			[&tokens](const std::string& alias) { return EqualsIgnoreCase(alias, tokens.front()); });
		if (!matches) {
			return s2sdk::ResultType::Continue;
		}
		OnMainCommand(caller, content.c_str());
		return s2sdk::ResultType::Handled; // the dot command is not chat
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

void RegisterSayListeners() {
	if (__s2sdk_AddCommandListener == nullptr || __s2sdk_RemoveCommandListener == nullptr) {
		LogError(std::string("[") + kLogTag +
				 "] the s2sdk command-listener bindings are unavailable - the '.' chat alias is "
				 "disabled");
		return;
	}
	for (const char* name : {"say", "say_team"}) {
		ListenerRegistration registration;
		registration.name = name;
		registration.callback = &OnSayListener;
		try {
			registration.registered =
				s2sdk::AddCommandListener(ToPlg(name), &OnSayListener, s2sdk::HookMode::Pre);
		} catch (...) {
			registration.registered = false;
		}
		if (registration.registered) {
			g_listeners.push_back(std::move(registration));
		} else {
			LogWarning(std::string("[") + kLogTag + "] could not hook '" + name + "' for '.' commands");
		}
	}
}

void RegisterCommandName(const std::string& name, s2sdk::ConCommandCallback callback) {
	if (name.empty() || callback == nullptr) {
		return;
	}
	for (const CommandRegistration& existing : g_commands) {
		if (EqualsIgnoreCase(existing.name, name)) {
			return; // one s2sdk name per registration, like pisexcompat's first-wins map
		}
	}
	// A7: without the removers the callback would point into the unloaded library. The ownership
	// is only known after AddConsoleCommand (true = created, false = a listener on somebody
	// else's name), so all three bindings are required before the name is touched.
	if (__s2sdk_AddConsoleCommand == nullptr || __s2sdk_RemoveCommand == nullptr ||
		__s2sdk_RemoveCommandListener == nullptr) {
		LogError(std::string("[") + kLogTag +
				 "] the s2sdk command bindings are unavailable (AddConsoleCommand/RemoveCommand/"
				 "RemoveCommandListener are required) - '" + name + "' is not registered");
		return;
	}
	CommandRegistration registration;
	registration.name = name;
	registration.callback = callback;
	registration.attempted = true;
	const int64_t flags = static_cast<int64_t>(s2sdk::ConVarFlag::Release)
						  | static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute);
	try {
		registration.owned = s2sdk::AddConsoleCommand(ToPlg(name), ToPlg(""),
													  static_cast<s2sdk::ConVarFlag>(flags), callback,
													  s2sdk::HookMode::Pre);
	} catch (...) {
		// s2sdk attaches the callback before it knows whether the name was free; the safe
		// assumption is "a listener of ours may be attached" -> tear down with the listener remover.
		registration.owned = false;
	}
	g_commands.push_back(std::move(registration));
}

void RegisterCommands() {
	// Console aliases first, then the trigger-less chat names, then the hard-coded "gc" command:
	// pisexcompat registered the names in this order and the first registration of a name wins.
	// pisexcompat lower-cased every registered name (its lookup map was case-insensitive), so a
	// config spelling such as "Clans" still answers "!clans".
	for (const std::string& alias : g_config.consoleAliases) {
		RegisterCommandName(LowerAscii(TrimSpaces(alias)), &OnMainCommandCallback);
	}
	for (const std::string& alias : g_config.chatAliases) {
		const std::string name = LowerAscii(StripChatTrigger(TrimSpaces(alias)));
		if (!name.empty() && name.front() != '.') {
			RegisterCommandName(name, &OnMainCommandCallback);
		}
	}
	if (g_config.guildChat) {
		RegisterCommandName("gc", &OnGuildChatCommandCallback);
	}
	// The old config's ".clan" alias (and any other dot alias) needs the say listener.
	const bool dotAlias = std::any_of(g_config.chatAliases.begin(), g_config.chatAliases.end(),
									  [](const std::string& alias) {
										  return !alias.empty() && alias.front() == '.';
									  });
	if (dotAlias) {
		RegisterSayListeners();
	}
}

bool OnMainCommand(int slot, const char* content) {
	try {
		const std::vector<std::string> tokens = TokenizeCommand(content != nullptr ? content : "");
		if (tokens.size() <= 1) {
			// Bare !clan / /clan / .clan opens the menu; the text subcommands stay available.
			if (slot >= 0) {
				if (!CommandThrottled(slot)) {
					OpenMainMenu(slot);
				}
			} else {
				CmdHelp(slot);
			}
			return true;
		}
		Subcommand command = ResolveSubcommand(tokens[1]);
		std::vector<std::string> args;
		if (command == Subcommand::Unknown) {
			command = Subcommand::Help;
		} else {
			args.assign(tokens.begin() + 2, tokens.end());
		}
		// Every subcommand except the local help text hits the database; one keypress is allowed
		// per second so a bound command cannot queue load after load on the single worker. Guild
		// chat is exempt (a dropped chat line is a lost message), and a dropped DB-backed
		// subcommand says why instead of doing nothing.
		if (command != Subcommand::Help && command != Subcommand::Chat && slot >= 0
			&& CommandThrottled(slot)) {
			Reply(slot, "k4.command.error.too_fast");
			return true;
		}
		Dispatch(slot, command, args);
	} catch (const std::exception& exception) {
		LogError(std::string("[") + kLogTag + "] command failed: " + exception.what());
	} catch (...) {
	}
	return true;
}

bool OnGuildChatCommand(int slot, const char* content) {
	try {
		if (slot < 0) {
			ReplyText(slot, Phrase("k4.command.error.player_only"));
			return true;
		}
		if (!g_config.guildChat) {
			Reply(slot, "k4.command.chat.disabled");
			return true;
		}
		const std::vector<std::string> tokens = TokenizeCommand(content != nullptr ? content : "");
		std::vector<std::string> args(tokens.size() > 1 ? tokens.begin() + 1 : tokens.end(), tokens.end());
		const PlayerIdentity identity = SlotIdentity(slot);
		if (!PlayerValid(slot, identity)) {
			return true;
		}
		CmdChat(slot, identity.steamid, args);
	} catch (...) {
	}
	return true;
}

void Dispatch(int slot, Subcommand command, const std::vector<std::string>& args) {
	if (slot < 0) {
		if (command == Subcommand::Help) {
			CmdHelp(slot);
		} else if (command == Subcommand::Top) {
			CmdTop(slot, args);
		} else {
			ReplyText(slot, Phrase("k4.command.error.player_only"));
		}
		return;
	}
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	switch (command) {
	case Subcommand::Help: CmdHelp(slot); break;
	case Subcommand::Info: CmdInfo(slot, identity.steamid); break;
	case Subcommand::Members: CmdMembers(slot, identity.steamid); break;
	case Subcommand::Top: CmdTop(slot, args); break;
	case Subcommand::Chat: CmdChat(slot, identity.steamid, args); break;
	case Subcommand::Unknown: CmdHelp(slot); break;
	}
}

void CmdHelp(int slot) {
	std::vector<std::string> lines;
	lines.push_back(WithPrefix(Phrase("k4.command.help.header")));
	lines.push_back(WithPrefix(Phrase("k4.command.help.info")));
	lines.push_back(WithPrefix(Phrase("k4.command.help.members")));
	lines.push_back(WithPrefix(Phrase("k4.command.help.top")));
	if (g_config.guildChat) {
		lines.push_back(WithPrefix(Phrase("k4.command.help.chat")));
	}
	lines.push_back(WithPrefix(Phrase("k4.command.help.footer")));
	ReplyLines(slot, lines);
}

void CmdInfo(int slot, uint64_t steamid) {
	(void)steamid;
	if (!g_store) {
		return;
	}
	WithGuild(slot, [slot](uint64_t, std::unique_ptr<Guild> guild) {
		if (!guild) {
			return;
		}
		const PlayerIdentity identity = SlotIdentity(slot);
		if (!identity.valid) {
			return;
		}
		const std::string auth = std::to_string(identity.steamid);
		const Member* member = FindMember(*guild, auth);
		const Role* role = member != nullptr ? FindRole(*guild, member->roleId) : nullptr;
		const LevelInfo level = DeriveLevel(guild->exp, g_config.levels);
		const std::string levelName =
			level.name.empty() ? FormatKey("k4.menu.level.default", {std::to_string(level.id)})
							   : level.name;
		std::vector<std::string> lines;
		lines.push_back(WithPrefix(FormatKey("k4.command.info.header", {guild->name, guild->tag})));
		lines.push_back(WithPrefix(FormatKey(
			"k4.command.info.members",
			{std::to_string(guild->members.size()),
			 std::to_string(EffectiveSlots(*guild, g_config.slotsStep))})));
		lines.push_back(WithPrefix(FormatKey("k4.command.info.bank", {std::to_string(guild->bank)})));
		lines.push_back(WithPrefix(FormatKey("k4.command.info.type", {GuildTypeName(guild->type)})));
		lines.push_back(WithPrefix(FormatKey("k4.command.info.exp", {std::to_string(guild->exp)})));
		lines.push_back(WithPrefix(FormatKey(
			"k4.command.info.level", {levelName, std::to_string(level.percent)})));
		lines.push_back(WithPrefix(FormatKey(
			"k4.command.info.role", {role != nullptr ? role->name : std::string("-")})));
		lines.push_back(WithPrefix(Phrase("k4.command.info.footer")));
		ReplyLines(slot, lines);
	});
}

void CmdMembers(int slot, uint64_t steamid) {
	(void)steamid;
	if (!g_store) {
		return;
	}
	WithGuild(slot, [slot](uint64_t, std::unique_ptr<Guild> guild) {
		if (!guild) {
			return;
		}
		std::vector<std::string> lines;
		lines.push_back(WithPrefix(Phrase("k4.command.members.header")));
		int online = 0;
		for (const Member& member : guild->members) {
			const Role* role = FindRole(*guild, member.roleId);
			std::optional<uint64_t> memberSteam;
			if (!member.auth.empty()) {
				char* end = nullptr;
				const unsigned long long value = std::strtoull(member.auth.c_str(), &end, 10);
				if (end != member.auth.c_str() && *end == '\0') {
					memberSteam = value;
				}
			}
			const bool isOnline = memberSteam.has_value() && FindSlotBySteam(*memberSteam) >= 0;
			if (isOnline) {
				++online;
			}
			lines.push_back(WithPrefix(FormatKey(
				"k4.command.members.item",
				{member.name, role != nullptr ? role->name : std::string("-"),
				 std::to_string(member.kills), std::to_string(member.deaths),
				 isOnline ? Phrase("k4.command.members.online") : std::string()})));
		}
		lines.push_back(WithPrefix(FormatKey(
			"k4.command.members.footer", {std::to_string(online), std::to_string(guild->members.size())})));
		ReplyLines(slot, lines);
	});
}

void CmdTop(int slot, const std::vector<std::string>& args) {
	if (!g_store) {
		return;
	}
	if (DatabaseBacklogged()) {
		Reply(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const std::string metric = args.empty() ? "exp" : LowerAscii(args[0]);
	bool enabled = false;
	if (metric == "bank") {
		enabled = g_config.topBank;
	} else if (metric == "exp") {
		enabled = g_config.topExp;
	} else if (metric == "kills") {
		enabled = g_config.topKills;
	} else if (metric == "deaths") {
		enabled = g_config.topDeaths;
	} else {
		Reply(slot, "k4.command.top.usage");
		return;
	}
	if (!enabled) {
		Reply(slot, "k4.command.top.disabled");
		return;
	}
	const PlayerIdentity identity = slot >= 0 ? SlotIdentity(slot) : PlayerIdentity{};
	const auto render = [slot, metric, identity](std::vector<TopEntry> entries) {
		if (slot >= 0 && (!identity.valid || !PlayerValid(slot, identity))) {
			return; // the DB round-trip outlived the player (disconnect / slot reuse)
		}
		std::vector<std::string> lines;
		lines.push_back(WithPrefix(FormatKey("k4.command.top.header", {metric})));
		if (entries.empty()) {
			lines.push_back(WithPrefix(Phrase("k4.command.top.empty")));
		}
		int position = 0;
		for (const TopEntry& entry : entries) {
			++position;
			lines.push_back(WithPrefix(FormatKey(
				"k4.command.top.item",
				{std::to_string(position), entry.name, std::to_string(static_cast<int64_t>(entry.value))})));
		}
		lines.push_back(WithPrefix(Phrase("k4.command.top.footer")));
		ReplyLines(slot, lines);
	};
	if (metric == "bank") {
		g_store->TopBank(10, render);
	} else if (metric == "exp") {
		g_store->TopExp(10, render);
	} else {
		g_store->TopKills(10, metric == "deaths", render);
	}
}

void CmdChat(int slot, uint64_t steamid, const std::vector<std::string>& args) {
	if (!g_config.guildChat) {
		Reply(slot, "k4.command.chat.disabled");
		return;
	}
	if (!g_store) {
		return;
	}
	const std::string message = TrimSpaces(JoinArgs(args, 0));
	if (message.empty()) {
		Reply(slot, "k4.command.usage.gc");
		return;
	}
	const PlayerIdentity identity = SlotIdentity(slot);
	const std::string senderName = ClientName(slot);
	WithGuild(slot, [slot, steamid, identity, message, senderName](uint64_t,
																  std::unique_ptr<Guild> guild) {
		if (!guild) {
			return;
		}
		if (FindMember(*guild, std::to_string(steamid)) == nullptr) {
			Reply(slot, "k4.guild.error.not_in_guild");
			return;
		}
		const std::string line = FormatKey("k4.chat.format", {guild->tag, senderName, message});
		for (const Member& member : guild->members) {
			char* end = nullptr;
			const unsigned long long value = std::strtoull(member.auth.c_str(), &end, 10);
			if (end == member.auth.c_str() || *end != '\0' || value == 0) {
				continue;
			}
			const int memberSlot = FindSlotBySteam(value);
			if (memberSlot < 0 || !PlayerValid(memberSlot, SlotIdentity(memberSlot))) {
				continue;
			}
			ReplyText(memberSlot, line);
		}
		(void)identity;
	});
}

// ---------------------------------------------------------------------------------------------
// Menu building (PITFALLS C10)
// ---------------------------------------------------------------------------------------------

void OpenMainMenu(int slot);
void OpenClanListMenu(int slot);
void OpenClanInfoMenu(int slot, int32_t guildId, std::unique_ptr<Guild> own);
void ShowClanInfoMenu(int slot, const PlayerIdentity& identity, const std::string& auth,
					  int32_t guildId, bool hasClan);
void OpenMembersMenu(int slot);
void OpenMemberCard(int slot, const std::string& auth);
void OpenTopMenu(int slot);
void OpenTopListMenu(int slot, const std::string& metric);
void OpenManagementMenu(int slot);
void OpenInviteMenu(int slot);
void OpenInviteConfirm(int slot, uint64_t target);
void OpenKickMenu(int slot);
void OpenKickConfirm(int slot, const std::string& auth);
void OpenRequestsMenu(int slot);
void OpenRequestConfirm(int slot, int64_t requestId, const std::string& auth, bool accept);
void OpenDisbandConfirm(int slot, int stage);
void OpenLeaveConfirm(int slot);
void OpenInvitePrompt(int slot, const ClanInvite& invite, const PlayerIdentity& targetIdentity);
void OpenConfirmMenu(int slot, const std::string& title, const std::string& yesAction);

void OpenMenuError(int slot, std::string_view key) {
	Reply(slot, key);
	OpenMainMenu(slot);
}

void OpenConfirmMenu(int slot, const std::string& title, const std::string& yesAction) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	std::vector<MenuEntry> items;
	items.push_back(MenuEntry{yesAction, Phrase("k4.menu.item.yes"), false});
	items.push_back(MenuEntry{"main", Phrase("k4.menu.item.no"), false});
	DisplayBuiltMenu(slot, identity, title, std::move(items), /*exitBack=*/false);
}

void OpenMainMenu(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		Reply(slot, "k4.menu.error.db_unavailable");
		return;
	}
	if (DatabaseBacklogged()) {
		Reply(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const uint64_t steamid = identity.steamid;
	const std::string auth = std::to_string(steamid);
	g_store->LoadGuildForPlayerChecked(
		auth, [slot, identity, auth, steamid](std::unique_ptr<Guild> guild,
											  const std::string& error) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!error.empty()) {
				// A failed query is not a kick: keep the cached membership and the tag.
				LogLoadError(error);
				Reply(slot, "k4.menu.error.db_unavailable");
				return;
			}
			if (guild) {
				PlayerInfo& info = g_players[steamid];
				info.steamid = steamid;
				info.slot = slot;
				info.auth = auth;
				info.clanId = guild->id;
				info.clanTag = guild->tag;
				info.loaded = true;
				const MemberMenuModel model = BuildMemberMenuModel(*guild, auth);
				std::vector<MenuEntry> items;
				items.push_back(MenuEntry{"info", Phrase("k4.menu.item.info"), false});
				items.push_back(MenuEntry{"members", Phrase("k4.menu.item.members"), false});
				items.push_back(MenuEntry{"top", Phrase("k4.menu.item.top_clans"), false});
				if (model.management) {
					items.push_back(MenuEntry{"management", Phrase("k4.menu.item.management"), false});
				}
				if (model.leave) {
					items.push_back(MenuEntry{"leave", Phrase("k4.menu.item.leave"), false});
				} else {
					items.push_back(MenuEntry{"", Phrase("k4.menu.item.owner_stays"), true});
				}
				DisplayBuiltMenu(slot, identity,
								 FormatKey("k4.menu.title.clan", {guild->tag}), std::move(items),
								 /*exitBack=*/false);
				return;
			}
			// No clan for this player (the site may have kicked/removed them since the last
			// refresh): update the cache now and clear a stale scoreboard tag left by a
			// membership the site ended.
			PlayerInfo& info = g_players[steamid];
			const bool hadGuild = info.clanId > 0 || !info.clanTag.empty();
			info.steamid = steamid;
			info.slot = slot;
			info.auth = auth;
			info.clanId = 0;
			info.clanTag.clear();
			info.loaded = true;
			if (hadGuild && g_config.showTagOnScoreboard) {
				ApplyScoreboardTag(slot, std::string());
			}
			std::vector<MenuEntry> items;
			const std::time_t now = std::time(nullptr);
			if (g_invites.Get(steamid, now).has_value()) {
				items.push_back(MenuEntry{"pending", Phrase("k4.menu.item.pending_invite"), false});
			}
			items.push_back(MenuEntry{"clans", Phrase("k4.menu.item.clans"), false});
			items.push_back(MenuEntry{"top", Phrase("k4.menu.item.top_clans"), false});
			DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.main"), std::move(items),
							 /*exitBack=*/false);
		});
}

void OpenClanListMenu(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	g_store->LoadClanList(30, [slot, identity](std::vector<ClanSummary> clans) {
		if (!PlayerValid(slot, identity)) {
			return;
		}
		std::vector<MenuEntry> items;
		if (clans.empty()) {
			items.push_back(MenuEntry{"", Phrase("k4.menu.item.no_clans"), true});
		}
		for (const ClanSummary& clan : clans) {
			const int32_t effective = clan.slots + clan.slotsUp * g_config.slotsStep;
			const std::string text = FormatKey(
				"k4.menu.clans.item",
				{clan.name, clan.tag, Phrase(std::string("k4.menu.type.") + GuildTypeName(clan.type)),
				 std::to_string(clan.members), std::to_string(effective)});
			items.push_back(MenuEntry{"clan:" + std::to_string(clan.id), text, false});
		}
		items.push_back(MenuEntry{"main", Phrase("k4.menu.item.back"), false});
		DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.clans"), std::move(items),
						 /*exitBack=*/false);
	});
}

void OpenClanInfoMenu(int slot, int32_t guildId, std::unique_ptr<Guild> own) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity) || !g_store) {
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	if (own) {
		// The "info" action already loaded this player's guild for its menu: reuse the snapshot
		// instead of running the five-query LoadGuildForPlayer a second time.
		ShowClanInfoMenu(slot, identity, auth, guildId, true);
		return;
	}
	g_store->LoadGuildForPlayerChecked(
		auth, [slot, identity, auth, guildId](std::unique_ptr<Guild> current,
											  const std::string& error) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!error.empty()) {
				LogLoadError(error);
				Reply(slot, "k4.menu.error.db_unavailable");
				return;
			}
			ShowClanInfoMenu(slot, identity, auth, guildId, current != nullptr);
		});
}

void ShowClanInfoMenu(int slot, const PlayerIdentity& identity, const std::string& auth,
					  int32_t guildId, bool hasClan) {
	if (!g_store) {
		return;
	}
	// The snapshot is only read while the callback runs, so the display step takes a raw pointer
	// and the caller keeps the ownership.
	const auto display = [slot, identity, hasClan](const Guild* guild, bool hasRequest) {
		if (!PlayerValid(slot, identity)) {
			return;
		}
		if (!guild) {
			OpenMenuError(slot, "k4.menu.error.clan_gone");
			return;
		}
		const LevelInfo level = DeriveLevel(guild->exp, g_config.levels);
		const std::string levelName =
			level.name.empty() ? FormatKey("k4.menu.level.default", {std::to_string(level.id)})
							   : level.name;
		const Member* owner = FindMember(*guild, guild->ownerAuth);
		std::vector<MenuEntry> items;
		// The action is the first item so it stays on the first page of the paginated menu.
		switch (DetermineJoinAction(guild->type, hasClan, hasRequest)) {
		case JoinAction::JoinOpen:
			items.push_back(
				MenuEntry{"join:" + std::to_string(guild->id), Phrase("k4.menu.item.join"), false});
			break;
		case JoinAction::Apply:
			items.push_back(
				MenuEntry{"apply:" + std::to_string(guild->id), Phrase("k4.menu.item.apply"), false});
			break;
		case JoinAction::CancelRequest:
			items.push_back(MenuEntry{"cancel:" + std::to_string(guild->id),
									  Phrase("k4.menu.item.cancel_request"), false});
			break;
		case JoinAction::InviteOnly:
			items.push_back(MenuEntry{"", Phrase("k4.menu.item.invite_only"), true});
			break;
		case JoinAction::None:
			break;
		}
		const auto line = [&items](const std::string& text) {
			items.push_back(MenuEntry{"", text, true});
		};
		line(FormatKey("k4.menu.info.name", {guild->name, guild->tag}));
		line(FormatKey("k4.menu.info.type",
					   {Phrase(std::string("k4.menu.type.") + GuildTypeName(guild->type))}));
		line(FormatKey("k4.menu.info.level", {std::to_string(guild->exp), levelName,
											  std::to_string(level.percent)}));
		line(FormatKey("k4.menu.info.bank", {std::to_string(guild->bank)}));
		line(FormatKey("k4.menu.info.slots", {std::to_string(guild->members.size()),
											  std::to_string(EffectiveSlots(*guild, g_config.slotsStep))}));
		line(FormatKey("k4.menu.info.owner", {owner != nullptr ? owner->name : guild->ownerAuth}));
		line(FormatKey("k4.menu.info.created", {FormatTime(guild->created)}));
		items.push_back(MenuEntry{hasClan ? "main" : "clans", Phrase("k4.menu.item.back"), false});
		DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.info"), std::move(items),
						 /*exitBack=*/false);
	};
	g_store->LoadGuild(guildId, [slot, identity, auth, hasClan, display](
									std::unique_ptr<Guild> guild) {
		if (!PlayerValid(slot, identity)) {
			return;
		}
		if (!guild) {
			display(nullptr, false);
			return;
		}
		if (hasClan) {
			// DetermineJoinAction() never consults the pending request for a member: skip the
			// extra round trip.
			display(guild.get(), false);
			return;
		}
		// Take the id and the retained guild in one evaluation-safe step: the former
		// `HasPendingRequest(guild->id, auth, [guild = shared_ptr(std::move(guild))])`
		// dereferenced guild after the lambda capture had moved it (argument evaluation order is
		// unspecified and GCC builds the capture first), crashing in Guild::id.
		GuildHandoff loaded = TakeLoadedGuild(std::move(guild));
		g_store->HasPendingRequest(
			loaded.guildId, auth,
			[slot, identity, display, guild = std::move(loaded.guild)](bool hasRequest) {
				if (!PlayerValid(slot, identity)) {
					return;
				}
				display(guild.get(), hasRequest);
			});
	});
}

void OpenMembersMenu(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	g_store->LoadGuildForPlayer(
		std::to_string(identity.steamid), [slot, identity](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			std::vector<MenuEntry> items;
			for (const Member& member : guild->members) {
				const Role* role = FindRole(*guild, member.roleId);
				const std::string text = FormatKey(
					"k4.menu.members.item",
					{member.name, role != nullptr ? role->name : std::string("-")});
				items.push_back(MenuEntry{"member:" + member.auth, text, false});
			}
			items.push_back(MenuEntry{"main", Phrase("k4.menu.item.back"), false});
			DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.members"), std::move(items),
							 /*exitBack=*/false);
		});
}

void OpenMemberCard(int slot, const std::string& auth) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity) || !g_store) {
		return;
	}
	g_store->LoadGuildForPlayer(
		std::to_string(identity.steamid),
		[slot, identity, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			const Member* member = FindMember(*guild, auth);
			if (member == nullptr) {
				OpenMenuError(slot, "k4.menu.error.player_left");
				return;
			}
			const Role* role = FindRole(*guild, member->roleId);
			std::vector<MenuEntry> items;
			const auto line = [&items](const std::string& text) {
				items.push_back(MenuEntry{"", text, true});
			};
			line(FormatKey("k4.menu.member.role", {role != nullptr ? role->name : std::string("-")}));
			line(FormatKey("k4.menu.member.kills", {std::to_string(member->kills)}));
			line(FormatKey("k4.menu.member.deaths", {std::to_string(member->deaths)}));
			line(FormatKey("k4.menu.member.joined", {FormatTime(member->joined)}));
			line(FormatKey("k4.menu.member.last_seen", {FormatTime(member->lastactivity)}));
			items.push_back(MenuEntry{"members", Phrase("k4.menu.item.back"), false});
			DisplayBuiltMenu(slot, identity, member->name, std::move(items), /*exitBack=*/false);
		});
}

void OpenTopMenu(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	std::vector<MenuEntry> items;
	if (g_config.topExp) {
		items.push_back(MenuEntry{"top:exp", Phrase("k4.menu.top.exp"), false});
	}
	if (g_config.topBank) {
		items.push_back(MenuEntry{"top:bank", Phrase("k4.menu.top.bank"), false});
	}
	if (g_config.topKills) {
		items.push_back(MenuEntry{"top:kills", Phrase("k4.menu.top.kills"), false});
	}
	if (g_config.topDeaths) {
		items.push_back(MenuEntry{"top:deaths", Phrase("k4.menu.top.deaths"), false});
	}
	items.push_back(MenuEntry{"main", Phrase("k4.menu.item.back"), false});
	DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.top"), std::move(items),
					 /*exitBack=*/false);
}

void OpenTopListMenu(int slot, const std::string& metric) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const auto render = [slot, identity, metric](std::vector<TopEntry> entries) {
		if (!PlayerValid(slot, identity)) {
			return;
		}
		std::vector<MenuEntry> items;
		if (entries.empty()) {
			items.push_back(MenuEntry{"", Phrase("k4.menu.item.no_data"), true});
		}
		int position = 0;
		for (const TopEntry& entry : entries) {
			++position;
			const std::string text = FormatKey(
				"k4.menu.top.item",
				{std::to_string(position), entry.name, std::to_string(static_cast<int64_t>(entry.value))});
			items.push_back(MenuEntry{"", text, true});
		}
		items.push_back(MenuEntry{"top", Phrase("k4.menu.item.back"), false});
		DisplayBuiltMenu(slot, identity, Phrase(std::string("k4.menu.top.") + metric),
						 std::move(items), /*exitBack=*/false);
	};
	if (metric == "bank") {
		g_store->TopBank(10, render);
	} else if (metric == "kills" || metric == "deaths") {
		g_store->TopKills(10, metric == "deaths", render);
	} else {
		g_store->TopExp(10, render);
	}
}

void OpenManagementMenu(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			const MemberMenuModel model = BuildMemberMenuModel(*guild, auth);
			if (!model.management) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			std::vector<MenuEntry> items;
			if (model.invite) {
				items.push_back(MenuEntry{"invite", Phrase("k4.menu.item.invite"), false});
			}
			if (model.kick) {
				items.push_back(MenuEntry{"kick", Phrase("k4.menu.item.kick"), false});
			}
			if (model.requests) {
				items.push_back(MenuEntry{"requests", Phrase("k4.menu.item.requests"), false});
			}
			if (model.disband) {
				items.push_back(MenuEntry{"disband", Phrase("k4.menu.item.disband"), false});
			}
			items.push_back(MenuEntry{"main", Phrase("k4.menu.item.back"), false});
			DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.management"), std::move(items),
							 /*exitBack=*/false);
		});
}

void OpenInviteMenu(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const uint64_t steamid = identity.steamid;
	const std::string auth = std::to_string(steamid);
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, steamid, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, auth, ClanRight::Invite)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			if (static_cast<int32_t>(guild->members.size())
				>= EffectiveSlots(*guild, g_config.slotsStep)) {
				OpenMenuError(slot, "k4.menu.error.clan_full");
				return;
			}
			std::vector<MenuEntry> items;
			const int maxSlots = std::min(kMaxSlots, MaxClients());
			int count = 0;
			for (int targetSlot = 0; targetSlot < maxSlots; ++targetSlot) {
				if (!HasController(targetSlot) || IsBotSlot(targetSlot)) {
					continue;
				}
				const uint64_t target = ClientSteamID(targetSlot);
				if (target == 0 || target == steamid) {
					continue;
				}
				const PlayerInfo* info = FindBySteam(target);
				if (info == nullptr || !info->loaded || info->clanId > 0) {
					continue;
				}
				items.push_back(MenuEntry{"invite:" + std::to_string(target),
										  ClientName(targetSlot), false});
				++count;
			}
			if (count == 0) {
				items.push_back(MenuEntry{"", Phrase("k4.menu.item.no_players"), true});
			}
			items.push_back(MenuEntry{"management", Phrase("k4.menu.item.back"), false});
			DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.invite"), std::move(items),
							 /*exitBack=*/false);
		});
}

void OpenInviteConfirm(int slot, uint64_t target) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity) || !g_store) {
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, auth, target](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, auth, ClanRight::Invite)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			const int targetSlot = FindSlotBySteam(target);
			if (targetSlot < 0) {
				OpenMenuError(slot, "k4.menu.error.player_left");
				return;
			}
			OpenConfirmMenu(slot, FormatKey("k4.menu.confirm.invite", {ClientName(targetSlot)}),
							"invite_do:" + std::to_string(target));
		});
}

void OpenKickMenu(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, auth, ClanRight::Kick)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			std::vector<MenuEntry> items;
			int count = 0;
			for (const Member& member : guild->members) {
				if (member.auth == auth || member.auth == guild->ownerAuth) {
					continue;
				}
				items.push_back(MenuEntry{"kick:" + member.auth, member.name, false});
				++count;
			}
			if (count == 0) {
				items.push_back(MenuEntry{"", Phrase("k4.menu.item.no_members"), true});
			}
			items.push_back(MenuEntry{"management", Phrase("k4.menu.item.back"), false});
			DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.kick"), std::move(items),
							 /*exitBack=*/false);
		});
}

void OpenKickConfirm(int slot, const std::string& auth) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity) || !g_store) {
		return;
	}
	const std::string actorAuth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		actorAuth, [slot, identity, actorAuth, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, actorAuth, ClanRight::Kick)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			const Member* member = FindMember(*guild, auth);
			if (member == nullptr) {
				OpenMenuError(slot, "k4.menu.error.player_left");
				return;
			}
			OpenConfirmMenu(slot, FormatKey("k4.menu.confirm.kick", {member->name}),
							"kick_do:" + auth);
		});
}

void OpenRequestsMenu(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, auth, ClanRight::Requests)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			const int32_t guildId = guild->id;
			g_store->LoadRequests(
				guildId, [slot, identity, guildId](std::vector<ClanRequest> requests) {
					if (!PlayerValid(slot, identity)) {
						return;
					}
					std::vector<MenuEntry> items;
					if (requests.empty()) {
						items.push_back(MenuEntry{"", Phrase("k4.menu.item.no_requests"), true});
					}
					for (const ClanRequest& request : requests) {
						const std::string text =
							FormatKey("k4.menu.requests.item", {request.name, request.auth});
						items.push_back(MenuEntry{"request:" + std::to_string(request.id) + ":"
													 + request.auth,
												 text, false});
					}
					items.push_back(MenuEntry{"management", Phrase("k4.menu.item.back"), false});
					DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.requests"),
									 std::move(items), /*exitBack=*/false);
				});
		});
}

void OpenRequestConfirm(int slot, int64_t requestId, const std::string& auth, bool accept) {
	(void)accept;
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const std::string actorAuth = std::to_string(identity.steamid);
	// The list this action came from may be stale (the site may have demoted/kicked the actor in
	// the meantime); re-check the request right against a freshly loaded guild before offering
	// the accept/decline menu. The write handlers check again as well.
	g_store->LoadGuildForPlayer(
		actorAuth, [slot, identity, actorAuth, requestId, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, actorAuth, ClanRight::Requests)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			std::vector<MenuEntry> items;
			items.push_back(MenuEntry{"req_accept:" + std::to_string(requestId) + ":" + auth,
									  Phrase("k4.menu.item.accept"), false});
			items.push_back(MenuEntry{"req_decline:" + std::to_string(requestId) + ":" + auth,
									  Phrase("k4.menu.item.decline"), false});
			items.push_back(MenuEntry{"requests", Phrase("k4.menu.item.back"), false});
			DisplayBuiltMenu(slot, identity, Phrase("k4.menu.title.request"), std::move(items),
							 /*exitBack=*/false);
		});
}

void OpenDisbandConfirm(int slot, int stage) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, auth, stage](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!IsOwner(*guild, auth)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			OpenConfirmMenu(slot, Phrase(stage <= 1 ? "k4.menu.confirm.disband1"
													: "k4.menu.confirm.disband2"),
							stage <= 1 ? "disband2" : "disband_do");
		});
}

void OpenLeaveConfirm(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	if (!g_store) {
		OpenMenuError(slot, "k4.menu.error.db_unavailable");
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (IsOwner(*guild, auth)) {
				OpenMenuError(slot, "k4.menu.error.owner_cannot_leave");
				return;
			}
			OpenConfirmMenu(slot, Phrase("k4.menu.confirm.leave"), "leave_do");
		});
}

void OpenInvitePrompt(int slot, const ClanInvite& invite, const PlayerIdentity& targetIdentity) {
	if (!g_store || !PlayerValid(slot, targetIdentity)) {
		return;
	}
	g_store->LoadGuild(invite.guildId, [slot, targetIdentity, invite](std::unique_ptr<Guild> guild) {
		if (!PlayerValid(slot, targetIdentity)) {
			return; // the invite menu must never open for a different player on that slot
		}
		if (!guild) {
			OpenMenuError(slot, "k4.menu.error.clan_gone");
			return;
		}
		std::vector<MenuEntry> items;
		items.push_back(MenuEntry{"invacc:" + std::to_string(guild->id),
								  Phrase("k4.menu.item.accept"), false});
		items.push_back(MenuEntry{"invdec:" + std::to_string(guild->id),
								  Phrase("k4.menu.item.decline"), false});
		DisplayBuiltMenu(slot, targetIdentity,
						 FormatKey("k4.menu.title.invite_prompt", {guild->name, guild->tag}),
						 std::move(items), /*exitBack=*/false);
		(void)invite;
	});
}
// ---------------------------------------------------------------------------------------------
// Menu actions
// ---------------------------------------------------------------------------------------------

void DoJoin(int slot, int32_t guildId);
void DoApply(int slot, int32_t guildId);
void DoCancelRequest(int slot, int32_t guildId);
void DoLeave(int slot);
void DoKick(int slot, const std::string& targetAuth);
void DoAcceptRequest(int slot, int64_t requestId, const std::string& auth);
void DoDeclineRequest(int slot, int64_t requestId, const std::string& auth);
void DoDisband(int slot);
void DoInvite(int slot, uint64_t target);
void DoAcceptInvite(int slot, int32_t guildId);
void DoDeclineInvite(int slot, int32_t guildId);

// Runs from OnPluginUpdate (DrainActions), never from inside the menu handler; the identity of the
// player who selected the item was re-validated by the caller.
void HandleMenuAction(int slot, const std::string& action) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	// One menu selection per second: the hud menu stays open after a selection, so a held key
	// would otherwise queue a full load per repeat on the single database worker. A separate
	// stamp from the command throttle keeps the first selection right after !clan responsive.
	if (MenuThrottled(slot)) {
		return;
	}
	const uint64_t steamid = identity.steamid;
	if (steamid == 0) {
		return;
	}
	const std::string auth = std::to_string(steamid);
	const auto starts = [&action](std::string_view prefix) {
		return action.rfind(prefix, 0) == 0;
	};
	const auto number = [](const std::string& text, std::size_t from) -> long long {
		if (from >= text.size()) {
			return 0;
		}
		char* end = nullptr;
		const long long value = std::strtoll(text.c_str() + from, &end, 10);
		return end == text.c_str() + from ? 0 : value;
	};
	const auto steam = [](const std::string& text, std::size_t from) -> uint64_t {
		if (from >= text.size()) {
			return 0;
		}
		char* end = nullptr;
		const unsigned long long value = std::strtoull(text.c_str() + from, &end, 10);
		return end == text.c_str() + from ? 0 : value;
	};

	if (action == "main") {
		OpenMainMenu(slot);
	} else if (action == "clans") {
		OpenClanListMenu(slot);
	} else if (action == "pending") {
		const std::optional<ClanInvite> invite = g_invites.Get(steamid, std::time(nullptr));
		if (invite.has_value()) {
			OpenInvitePrompt(slot, *invite, identity);
		} else {
			OpenMenuError(slot, "k4.menu.error.invite_expired");
		}
	} else if (action == "top") {
		OpenTopMenu(slot);
	} else if (starts("top:")) {
		OpenTopListMenu(slot, action.substr(4));
	} else if (action == "info") {
		if (!g_store) {
			OpenMenuError(slot, "k4.menu.error.db_unavailable");
			return;
		}
		g_store->LoadGuildForPlayerChecked(
			auth, [slot, identity](std::unique_ptr<Guild> guild, const std::string& error) {
				if (!PlayerValid(slot, identity)) {
					return;
				}
				if (!error.empty()) {
					LogLoadError(error);
					Reply(slot, "k4.menu.error.db_unavailable");
					return;
				}
				if (!guild) {
					OpenMenuError(slot, "k4.menu.error.not_in_clan");
					return;
				}
				// Read the id before the guild is moved into the call (argument evaluation order
				// is unspecified: `OpenClanInfoMenu(slot, guild->id, std::move(guild))` can
				// dereference the null pointer).
				const int32_t guildId = guild->id;
				OpenClanInfoMenu(slot, guildId, std::move(guild));
			});
	} else if (action == "members") {
		OpenMembersMenu(slot);
	} else if (starts("member:")) {
		OpenMemberCard(slot, action.substr(7));
	} else if (action == "management") {
		OpenManagementMenu(slot);
	} else if (action == "invite") {
		OpenInviteMenu(slot);
	} else if (starts("invite_do:")) {
		DoInvite(slot, steam(action, 10));
	} else if (starts("invite:")) {
		OpenInviteConfirm(slot, steam(action, 7));
	} else if (action == "kick") {
		OpenKickMenu(slot);
	} else if (starts("kick_do:")) {
		DoKick(slot, action.substr(8));
	} else if (starts("kick:")) {
		OpenKickConfirm(slot, action.substr(5));
	} else if (action == "requests") {
		OpenRequestsMenu(slot);
	} else if (starts("req_accept:")) {
		const std::size_t separator = action.find(':', 11);
		DoAcceptRequest(slot, number(action, 11), separator == std::string::npos
														 ? std::string()
														 : action.substr(separator + 1));
	} else if (starts("req_decline:")) {
		const std::size_t separator = action.find(':', 12);
		DoDeclineRequest(slot, number(action, 12), separator == std::string::npos
														  ? std::string()
														  : action.substr(separator + 1));
	} else if (starts("request:")) {
		const std::size_t separator = action.find(':', 8);
		const long long requestId = number(action, 8);
		const std::string target = separator == std::string::npos ? std::string()
																  : action.substr(separator + 1);
		OpenRequestConfirm(slot, requestId, target, true);
	} else if (action == "disband") {
		OpenDisbandConfirm(slot, 1);
	} else if (action == "disband2") {
		OpenDisbandConfirm(slot, 2);
	} else if (action == "disband_do") {
		DoDisband(slot);
	} else if (action == "leave") {
		OpenLeaveConfirm(slot);
	} else if (action == "leave_do") {
		DoLeave(slot);
	} else if (starts("clan:")) {
		OpenClanInfoMenu(slot, static_cast<int32_t>(number(action, 5)), nullptr);
	} else if (starts("join:")) {
		DoJoin(slot, static_cast<int32_t>(number(action, 5)));
	} else if (starts("apply:")) {
		DoApply(slot, static_cast<int32_t>(number(action, 6)));
	} else if (starts("cancel:")) {
		DoCancelRequest(slot, static_cast<int32_t>(number(action, 7)));
	} else if (starts("invacc:")) {
		DoAcceptInvite(slot, static_cast<int32_t>(number(action, 7)));
	} else if (starts("invdec:")) {
		DoDeclineInvite(slot, static_cast<int32_t>(number(action, 7)));
	}
}

void DrainActions() {
	while (!g_pendingActions.empty()) {
		PendingAction action = std::move(g_pendingActions.front());
		g_pendingActions.pop_front();
		if (!CanRunAction(action)) {
			continue;
		}
		try {
			action.fn();
		} catch (const std::exception& exception) {
			LogError(std::string("[") + kLogTag + "] queued action failed: " + exception.what());
		} catch (...) {
			LogError(std::string("[") + kLogTag + "] queued action failed with an unknown exception");
		}
	}
}

void DoJoin(int slot, int32_t guildId) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	const std::string name = ClientName(slot);
	g_store->LoadGuild(
		guildId, [slot, identity, auth, name, guildId](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.clan_gone");
				return;
			}
			if (guild->type != 1) {
				OpenMenuError(slot, "k4.menu.error.not_open");
				return;
			}
			if (static_cast<int32_t>(guild->members.size())
				>= EffectiveSlots(*guild, g_config.slotsStep)) {
				OpenMenuError(slot, "k4.menu.error.clan_full");
				return;
			}
			const std::string guildName = guild->name;
			g_store->LoadGuildForPlayer(
				auth, [slot, identity, auth, name, guildId, guildName](std::unique_ptr<Guild> current) {
					if (!PlayerValid(slot, identity)) {
						return;
					}
					if (current) {
						OpenMenuError(slot, "k4.menu.error.already_in_clan");
						return;
					}
					g_store->JoinClan(guildId, auth, name, g_config.slotsStep, true,
									  [slot, identity, auth, guildId, guildName](bool ok) {
										  if (ok) {
											  LogAction(guildId, "join", LogValuePlayer(auth));
										  }
										  if (!PlayerValid(slot, identity)) {
											  return; // the player left while the write was in flight
										  }
										  if (!ok) {
											  OpenMenuError(slot, "k4.menu.error.join_failed");
											  return;
										  }
										  Reply(slot, "k4.menu.chat.joined", {guildName});
										  EnsurePlayer(slot, identity.steamid, true);
										  OpenMainMenu(slot);
									  });
				});
		});
}

void DoApply(int slot, int32_t guildId) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	const std::string name = ClientName(slot);
	g_store->LoadGuild(
		guildId, [slot, identity, auth, name, guildId](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.clan_gone");
				return;
			}
			if (guild->type != 3) {
				OpenMenuError(slot, "k4.menu.error.not_request");
				return;
			}
			const std::string guildName = guild->name;
			g_store->LoadGuildForPlayer(
				auth, [slot, identity, auth, name, guildId, guildName](std::unique_ptr<Guild> current) {
					if (!PlayerValid(slot, identity)) {
						return;
					}
					if (current) {
						OpenMenuError(slot, "k4.menu.error.already_in_clan");
						return;
					}
					g_store->ApplyToClan(
						guildId, auth, name, [slot, identity, auth, guildId, guildName](bool ok) {
							if (ok) {
								LogAction(guildId, "request", LogValuePlayer(auth));
							}
							if (!PlayerValid(slot, identity)) {
								return; // the player left while the write was in flight
							}
							if (!ok) {
								OpenMenuError(slot, "k4.menu.error.already_requested");
								return;
							}
							Reply(slot, "k4.menu.chat.request_sent", {guildName});
							OpenMainMenu(slot);
						});
				});
		});
}

void DoCancelRequest(int slot, int32_t guildId) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	g_store->CancelRequest(
		guildId, auth, [slot, identity, auth, guildId](bool ok) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!ok) {
				OpenMenuError(slot, "k4.menu.error.no_request");
				return;
			}
			LogAction(guildId, "request_cancel", LogValuePlayer(auth));
			Reply(slot, "k4.menu.chat.request_cancelled");
			OpenMainMenu(slot);
		});
}

void DoLeave(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (IsOwner(*guild, auth)) {
				OpenMenuError(slot, "k4.menu.error.owner_cannot_leave");
				return;
			}
			const int32_t guildId = guild->id;
			const std::string guildName = guild->name;
			g_store->LeaveClan(
				guildId, auth, [slot, identity, auth, guildId, guildName](bool ok) {
					if (ok) {
						LogAction(guildId, "exit", LogValuePlayer(auth));
						ClearMemberCacheAndTag(identity.steamid); // steam-keyed: valid after a disconnect
					}
					if (!PlayerValid(slot, identity)) {
						return; // the player left while the write was in flight
					}
					if (!ok) {
						OpenMenuError(slot, "k4.menu.error.player_left");
						return;
					}
					Reply(slot, "k4.menu.chat.left", {guildName});
					OpenMainMenu(slot);
				});
		});
}

void DoKick(int slot, const std::string& targetAuth) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const std::string actorAuth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		actorAuth, [slot, identity, actorAuth, targetAuth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, actorAuth, ClanRight::Kick)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			const Member* target = FindMember(*guild, targetAuth);
			if (target == nullptr) {
				OpenMenuError(slot, "k4.menu.error.player_left");
				return;
			}
			if (targetAuth == guild->ownerAuth) {
				OpenMenuError(slot, "k4.menu.error.cannot_kick_owner");
				return;
			}
			if (targetAuth == actorAuth) {
				OpenMenuError(slot, "k4.menu.error.cannot_kick_self");
				return;
			}
			const int32_t guildId = guild->id;
			const std::string targetName = target->name;
			uint64_t targetSteam = 0;
			{
				char* end = nullptr;
				const unsigned long long value = std::strtoull(targetAuth.c_str(), &end, 10);
				if (end != targetAuth.c_str() && *end == '\0') {
					targetSteam = value;
				}
			}
			g_store->KickMember(
				guildId, actorAuth, targetAuth,
				[slot, identity, actorAuth, targetAuth, targetName, targetSteam, guildId](bool ok) {
					if (ok) {
						LogAction(guildId, "kick", LogValueKick(targetAuth, ""));
						const int targetSlot = FindSlotBySteam(targetSteam);
						if (targetSlot >= 0) {
							const PlayerIdentity targetIdentity = SlotIdentity(targetSlot);
							if (PlayerValid(targetSlot, targetIdentity)) {
								Reply(targetSlot, "k4.menu.chat.kicked_target");
							}
						}
						ClearMemberCacheAndTag(targetSteam);
					}
					if (!PlayerValid(slot, identity)) {
						return; // the actor left while the write was in flight
					}
					if (!ok) {
						OpenMenuError(slot, "k4.menu.error.player_left");
						return;
					}
					Reply(slot, "k4.menu.chat.kicked_actor", {targetName});
					OpenManagementMenu(slot);
				});
			(void)actorAuth;
		});
}

void DoAcceptRequest(int slot, int64_t requestId, const std::string& auth) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const std::string actorAuth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		actorAuth, [slot, identity, actorAuth, requestId, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, actorAuth, ClanRight::Requests)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			if (static_cast<int32_t>(guild->members.size()) >= EffectiveSlots(*guild, g_config.slotsStep)) {
				OpenMenuError(slot, "k4.menu.error.clan_full");
				return;
			}
			const int32_t guildId = guild->id;
			uint64_t targetSteam = 0;
			{
				char* end = nullptr;
				const unsigned long long value = std::strtoull(auth.c_str(), &end, 10);
				if (end != auth.c_str() && *end == '\0') {
					targetSteam = value;
				}
			}
			g_store->AcceptRequest(
				guildId, requestId, auth, g_config.slotsStep,
				[slot, identity, auth, guildId, targetSteam](bool ok) {
					if (ok) {
						LogAction(guildId, "request_accept", LogValuePlayer(auth));
						const int targetSlot = FindSlotBySteam(targetSteam);
						if (targetSlot >= 0) {
							const PlayerIdentity targetIdentity = SlotIdentity(targetSlot);
							if (PlayerValid(targetSlot, targetIdentity)) {
								Reply(targetSlot, "k4.menu.chat.joined_by_request");
								EnsurePlayer(targetSlot, targetSteam, true);
							}
						}
					}
					if (!PlayerValid(slot, identity)) {
						return; // the actor left while the write was in flight
					}
					if (!ok) {
						OpenMenuError(slot, "k4.menu.error.request_gone");
						return;
					}
					Reply(slot, "k4.menu.chat.request_accepted");
					OpenRequestsMenu(slot);
				});
		});
}

void DoDeclineRequest(int slot, int64_t requestId, const std::string& auth) {
	(void)requestId;
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const std::string actorAuth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		actorAuth, [slot, identity, actorAuth, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, actorAuth, ClanRight::Requests)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			const int32_t guildId = guild->id;
			g_store->DeclineRequest(
				guildId, auth, [slot, identity, auth, guildId](bool ok) {
					if (!PlayerValid(slot, identity)) {
						return;
					}
					if (!ok) {
						OpenMenuError(slot, "k4.menu.error.request_gone");
						return;
					}
					LogAction(guildId, "request_cancel", LogValuePlayer(auth));
					Reply(slot, "k4.menu.chat.request_declined");
					OpenRequestsMenu(slot);
				});
		});
}

void DoDisband(int slot) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, auth](std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!IsOwner(*guild, auth)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			const int32_t guildId = guild->id;
			std::vector<uint64_t> members;
			for (const auto& [memberSteam, info] : g_players) {
				if (info.clanId == guildId) {
					members.push_back(memberSteam);
				}
			}
			g_store->DisbandClan(
				guildId, auth, g_config.logs.deleteLogs,
				[slot, identity, guildId, members](bool ok) {
					if (!ok) {
						if (PlayerValid(slot, identity)) {
							OpenMenuError(slot, "k4.menu.error.db_unavailable");
						}
						return;
					}
					g_invites.RemoveGuild(guildId);
					for (const uint64_t memberSteam : members) {
						const int memberSlot = FindSlotBySteam(memberSteam);
						if (memberSlot >= 0) {
							const PlayerIdentity memberIdentity = SlotIdentity(memberSlot);
							if (PlayerValid(memberSlot, memberIdentity)) {
								Reply(memberSlot, "k4.menu.chat.disbanded");
							}
						}
						ClearMemberCacheAndTag(memberSteam);
					}
					if (!PlayerValid(slot, identity)) {
						return;
					}
					OpenMainMenu(slot);
				});
		});
}

void DoInvite(int slot, uint64_t target) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const uint64_t steamid = identity.steamid;
	const std::string auth = std::to_string(steamid);
	if (target == 0 || target == steamid) {
		OpenMenuError(slot, "k4.menu.error.cannot_invite_self");
		return;
	}
	const int targetSlot = FindSlotBySteam(target);
	if (targetSlot < 0) {
		OpenMenuError(slot, "k4.menu.error.player_left");
		return;
	}
	// The target's controller handle joins the SteamID in the identity: the invite prompt must
	// never open for a different player who got that slot before the queries finish.
	const PlayerIdentity targetIdentity = SlotIdentity(targetSlot);
	if (!targetIdentity.valid) {
		OpenMenuError(slot, "k4.menu.error.player_left");
		return;
	}
	g_store->LoadGuildForPlayer(
		auth, [slot, identity, auth, target, targetSlot, targetIdentity](
				  std::unique_ptr<Guild> guild) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!guild) {
				OpenMenuError(slot, "k4.menu.error.not_in_clan");
				return;
			}
			if (!HasClanRight(*guild, auth, ClanRight::Invite)) {
				OpenMenuError(slot, "k4.menu.error.no_rights");
				return;
			}
			// An invite into a full clan could never be accepted; refuse it before it reaches
			// the target (the accept path re-checks the cap against a fresh snapshot).
			if (static_cast<int32_t>(guild->members.size())
				>= EffectiveSlots(*guild, g_config.slotsStep)) {
				OpenMenuError(slot, "k4.menu.error.clan_full");
				return;
			}
			const int32_t guildId = guild->id;
			const std::string guildName = guild->name;
			const std::string senderName = ClientName(slot);
			g_store->LoadGuildForPlayer(
				std::to_string(target),
				[slot, identity, target, targetSlot, targetIdentity, guildId, guildName,
				 senderName](std::unique_ptr<Guild> current) {
					if (!PlayerValid(slot, identity)) {
						return;
					}
					if (current) {
						OpenMenuError(slot, "k4.menu.error.target_in_clan");
						return;
					}
					if (!PlayerValid(targetSlot, targetIdentity)) {
						OpenMenuError(slot, "k4.menu.error.player_left");
						return;
					}
					const std::time_t now = std::time(nullptr);
					if (!g_invites.Create(guildId, identity.steamid, target, now)) {
						OpenMenuError(slot, "k4.menu.error.invite_cooldown");
						return;
					}
					Reply(slot, "k4.menu.chat.invite_sent");
					Reply(targetSlot, "k4.menu.chat.invite_received", {senderName, guildName});
					const std::optional<ClanInvite> created =
						g_invites.Get(target, now);
					if (created.has_value()) {
						OpenInvitePrompt(targetSlot, *created, targetIdentity);
					}
					OpenManagementMenu(slot);
				});
		});
}

void DoAcceptInvite(int slot, int32_t guildId) {
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!g_store || !PlayerValid(slot, identity)) {
		return;
	}
	const std::string auth = std::to_string(identity.steamid);
	const std::string name = ClientName(slot);
	const std::time_t now = std::time(nullptr);
	g_store->LoadGuildChecked(
		guildId, [slot, identity, auth, name, guildId, now](std::unique_ptr<Guild> guild,
															const std::string& error) {
			if (!PlayerValid(slot, identity)) {
				return;
			}
			if (!error.empty()) {
				// A failed query is not a dead clan: keep the invite for the next attempt.
				LogLoadError(error);
				Reply(slot, "k4.menu.error.db_unavailable");
				return;
			}
			g_store->LoadGuildForPlayerChecked(
				auth, [slot, identity, auth, name, guildId, now,
					   guild = std::shared_ptr<Guild>(std::move(guild))](
						  std::unique_ptr<Guild> current, const std::string& loadError) {
					if (!PlayerValid(slot, identity)) {
						return;
					}
					if (!loadError.empty()) {
						LogLoadError(loadError);
						Reply(slot, "k4.menu.error.db_unavailable");
						return;
					}
					// The inviter may have lost the invite right (site demote/kick) since sending
					// the invite: re-check it on the freshly loaded guild before the accept.
					const std::optional<ClanInvite> invite =
						g_invites.Get(identity.steamid, now);
					if (guild != nullptr && invite.has_value() && invite->guildId == guildId
						&& !HasClanRight(*guild, std::to_string(invite->inviter), ClanRight::Invite)) {
						g_invites.Decline(identity.steamid);
						OpenMenuError(slot, "k4.menu.error.invite_revoked");
						return;
					}
					const InviteAcceptResult result = EvaluateInviteAccept(
						g_invites, identity.steamid, guildId, current != nullptr, guild != nullptr,
						guild != nullptr ? static_cast<int32_t>(guild->members.size()) : 0,
						guild != nullptr ? EffectiveSlots(*guild, g_config.slotsStep) : 0, now);
					switch (result) {
					case InviteAcceptResult::Expired:
						OpenMenuError(slot, "k4.menu.error.invite_expired");
						return;
					case InviteAcceptResult::ClanGone:
						g_invites.Decline(identity.steamid);
						OpenMenuError(slot, "k4.menu.error.clan_gone");
						return;
					case InviteAcceptResult::AlreadyInClan:
						g_invites.Decline(identity.steamid);
						OpenMenuError(slot, "k4.menu.error.already_in_clan");
						return;
					case InviteAcceptResult::Full:
						OpenMenuError(slot, "k4.menu.error.clan_full");
						return;
					case InviteAcceptResult::Ok:
						break;
					}
					const std::string guildName = guild->name;
					g_store->JoinClan(
						guildId, auth, name, g_config.slotsStep, false,
						[slot, identity, auth, guildId, guildName, now](bool ok) {
							if (ok) {
								// Consume the invite / write the log even if the player left during
								// the round-trip: the membership exists.
								g_invites.Accept(identity.steamid, guildId, now);
								LogAction(guildId, "join", LogValuePlayer(auth));
							}
							if (!PlayerValid(slot, identity)) {
								return;
							}
							if (!ok) {
								OpenMenuError(slot, "k4.menu.error.join_failed");
								return;
							}
							Reply(slot, "k4.menu.chat.joined", {guildName});
							EnsurePlayer(slot, identity.steamid, true);
							OpenMainMenu(slot);
						});
				});
		});
}

void DoDeclineInvite(int slot, int32_t guildId) {
	(void)guildId;
	const PlayerIdentity identity = SlotIdentity(slot);
	if (!PlayerValid(slot, identity)) {
		return;
	}
	g_invites.Decline(identity.steamid);
	Reply(slot, "k4.menu.chat.invite_declined");
	OpenMainMenu(slot);
}
// ---------------------------------------------------------------------------------------------
// Game events (PITFALLS C12: player fields only through GetEventPlayerSlot)
// ---------------------------------------------------------------------------------------------

int EventPlayerSlot(void* event, const char* key) {
	if (event == nullptr || key == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
		return -1;
	}
	try {
		return s2sdk::GetEventPlayerSlot(event, ToPlg(key));
	} catch (...) {
		return -1;
	}
}

void HandlePlayerActivate(void* event);
void HandlePlayerDisconnect(void* event);
void HandlePlayerDeath(void* event);
void HandlePlayerSpawn(void* event);
void HandlePlayerTeam(void* event);

void HandleGameEvent(std::string_view name, void* raw) {
	if (!g_mapReady || g_shuttingDown) {
		return; // no client/entity access between maps (PITFALLS B1/B2)
	}
	if (name == "player_activate") {
		HandlePlayerActivate(raw);
	} else if (name == "player_disconnect") {
		HandlePlayerDisconnect(raw);
	} else if (name == "player_death") {
		HandlePlayerDeath(raw);
	} else if (name == "player_spawn") {
		HandlePlayerSpawn(raw);
	} else if (name == "player_team") {
		HandlePlayerTeam(raw);
	}
}

s2sdk::ResultType OnGameEventTrampoline(const plg::string& name, void* event, bool /*dontBroadcast*/) {
	try {
		HandleGameEvent(ToStd(name), event);
	} catch (const std::exception& exception) {
		LogError(std::string("[") + kLogTag + "] game event handler failed: " + exception.what());
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] game event handler failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

void HandlePlayerActivate(void* raw) {
	const int slot = EventPlayerSlot(raw, "userid");
	if (!ValidSlot(slot) || !HasController(slot) || IsBotSlot(slot)) {
		return;
	}
	const uint64_t steamid = ClientSteamID(slot);
	if (steamid == 0) {
		return;
	}
	// After a map change the reconcile pass and player_activate can both fire for the same
	// player; the pass's load is already reading the current rows, so a second forced one only
	// doubles the queries. (A normal forced reload after a join/leave never races this way: the
	// load in flight then was started after the membership changed.)
	PlayerInfo* existing = FindBySteam(steamid);
	if (existing != nullptr && existing->loading) {
		existing->slot = slot;
		// Keep the slot->steamid map current: HandlePlayerDisconnect finds the cache entry to
		// erase through it, and this branch returns before any of the load paths set it.
		g_slotSteam[static_cast<std::size_t>(slot)] = steamid;
		return;
	}
	EnsurePlayer(slot, steamid, true);
}

void HandlePlayerDisconnect(void* raw) {
	const int slot = EventPlayerSlot(raw, "userid");
	if (!ValidSlot(slot)) {
		return;
	}
	const uint64_t steamid = g_slotSteam[static_cast<std::size_t>(slot)];
	g_slotSteam[static_cast<std::size_t>(slot)] = 0;
	if (steamid != 0) {
		g_invites.RemoveTarget(steamid);
		g_players.erase(steamid);
	}
}

void HandlePlayerSpawn(void* raw) {
	const int slot = EventPlayerSlot(raw, "userid");
	if (!ValidSlot(slot) || !HasController(slot) || IsBotSlot(slot)) {
		return;
	}
	const uint64_t steamid = ClientSteamID(slot);
	const PlayerInfo* info = steamid != 0 ? FindBySteam(steamid) : nullptr;
	if (info != nullptr && info->clanId > 0 && !info->clanTag.empty()) {
		ApplyScoreboardTag(slot, FormatClanTag(info->clanTag));
	}
}

void HandlePlayerTeam(void* raw) {
	const int slot = EventPlayerSlot(raw, "userid");
	if (!ValidSlot(slot) || !HasController(slot) || IsBotSlot(slot)) {
		return;
	}
	const uint64_t steamid = ClientSteamID(slot);
	const PlayerInfo* info = steamid != 0 ? FindBySteam(steamid) : nullptr;
	if (info != nullptr && info->clanId > 0 && !info->clanTag.empty()) {
		ApplyScoreboardTag(slot, FormatClanTag(info->clanTag));
	}
}

void HandlePlayerDeath(void* raw) {
	if (!g_store) {
		return; // no usable database entry: the runtime is disabled (do not crash on death)
	}
	const int attackerSlot = EventPlayerSlot(raw, "attacker");
	const int victimSlot = EventPlayerSlot(raw, "userid");
	// Bots have no SteamID and no clan (DESIGN §4: skip bots). By default a death a bot took
	// part in changes nothing at all - neither the human killer's bank/kills nor the human
	// victim's bank/deaths; kill_rewards.count_bots restores the old per-participant behaviour.
	const auto isBot = [](int slot) {
		return ValidSlot(slot) && HasController(slot) && IsBotSlot(slot);
	};
	if (IgnoreBotDeath(g_config.countBots, isBot(attackerSlot), isBot(victimSlot))) {
		return;
	}
	const uint64_t attacker = SlotSteam(attackerSlot);
	const uint64_t victim = SlotSteam(victimSlot);
	if (attacker == 0 && victim == 0) {
		return;
	}
	const bool samePlayer = attacker != 0 && attacker == victim;
	const int attackerTeam = attacker != 0 ? TeamOf(attackerSlot) : 0;
	const int victimTeam = victim != 0 ? TeamOf(victimSlot) : 0;
	const bool sameTeam = attackerTeam > 1 && attackerTeam == victimTeam;

	const PlayerInfo* attackerInfo = attacker != 0 ? FindBySteam(attacker) : nullptr;
	const PlayerInfo* victimInfo = victim != 0 ? FindBySteam(victim) : nullptr;
	const bool sameClan = attackerInfo != nullptr && victimInfo != nullptr && attackerInfo->clanId > 0
						  && attackerInfo->clanId == victimInfo->clanId;

	const KillDeltas deltas =
		ComputeKillDeltas(g_config, attacker != 0, victim != 0, samePlayer, sameTeam, sameClan, IsWarmup());

	const auto applyDelta = [](uint64_t steamid, int32_t delta) {
		if (steamid == 0 || delta == 0) {
			return;
		}
		const PlayerInfo* info = FindBySteam(steamid);
		if (info != nullptr && info->loaded) {
			if (info->clanId > 0) {
				g_store->AddBank(info->clanId, delta);
			}
			return;
		}
		g_store->LoadClanId(std::to_string(steamid), [delta](int32_t clanId) {
			if (clanId > 0) {
				g_store->AddBank(clanId, delta);
			}
		});
	};
	if (deltas.attacker) {
		applyDelta(attacker, deltas.attackerDelta);
	}
	if (deltas.victim) {
		applyDelta(victim, deltas.victimDelta);
	}
	g_store->AddKillStats(deltas.attacker ? std::to_string(attacker) : std::string(),
						  deltas.victim ? std::to_string(victim) : std::string());
}

void RegisterEvents() {
	// A7: both halves of the hook pair are required before anything is hooked; a missing
	// UnhookEvent would leave a callback pointing into the unloaded library after plg reload.
	if (__s2sdk_HookEvent == nullptr || __s2sdk_UnhookEvent == nullptr) {
		LogError(std::string("[") + kLogTag +
				 "] the s2sdk event bindings are unavailable (HookEvent/UnhookEvent are required) - "
				 "game events are not observed");
		return;
	}
	if (__s2sdk_GetEventPlayerSlot == nullptr) {
		LogError(std::string("[") + kLogTag +
				 "] the s2sdk player-slot accessor is unavailable - game events are not hooked");
		return;
	}
	for (const char* name : {"player_activate", "player_disconnect", "player_death", "player_spawn",
							 "player_team"}) {
		bool hooked = false;
		try {
			hooked = s2sdk::HookEvent(ToPlg(name), &OnGameEventTrampoline, s2sdk::HookMode::Pre) ==
					 s2sdk::EventHookError::Okay;
		} catch (...) {
			hooked = false;
		}
		if (hooked) {
			g_hookedEvents.emplace_back(name);
		} else {
			LogWarning(std::string("[") + kLogTag + "] cannot hook event " + name);
		}
	}
}

void RemoveEvents() {
	if (__s2sdk_UnhookEvent != nullptr) {
		for (const std::string& name : g_hookedEvents) {
			try {
				s2sdk::UnhookEvent(ToPlg(name), &OnGameEventTrampoline, s2sdk::HookMode::Pre);
			} catch (...) {
			}
		}
	}
	g_hookedEvents.clear();
}

void RemoveCommands() {
	for (const CommandRegistration& registration : g_commands) {
		try {
			if (registration.owned) {
				if (__s2sdk_RemoveCommand != nullptr) {
					s2sdk::RemoveCommand(ToPlg(registration.name), registration.callback);
				}
			} else if (__s2sdk_RemoveCommandListener != nullptr) {
				s2sdk::RemoveCommandListener(ToPlg(registration.name), registration.callback,
											 s2sdk::HookMode::Pre);
			}
		} catch (...) {
		}
	}
	g_commands.clear();
	for (const ListenerRegistration& listener : g_listeners) {
		if (__s2sdk_RemoveCommandListener != nullptr) {
			try {
				s2sdk::RemoveCommandListener(ToPlg(listener.name), listener.callback,
											 s2sdk::HookMode::Pre);
			} catch (...) {
			}
		}
	}
	g_listeners.clear();
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

void ResetRuntimeState() {
	g_players.clear();
	g_slotSteam.fill(0);
	g_nextGeneration = 0;
	g_reconcileInFlight = false;
	g_nextReconcilePass = 0;
	g_activeReconcilePass = 0;
	g_reconcileStartedAt = std::chrono::steady_clock::time_point{};
	g_lastLoadErrorLog = std::chrono::steady_clock::time_point{};
	g_lastCommandAt.fill(std::chrono::steady_clock::time_point{});
	g_lastMenuActionAt.fill(std::chrono::steady_clock::time_point{});
	g_invites = InviteRegistry{};
	g_phrases.clear();
	g_language = "en";

	g_pendingActions.clear();
	for (MenuSlot& menu : g_menus) {
		menu = MenuSlot{};
	}
	g_menuDestroy.clear();
	g_parkedMenus.clear();
	g_reservedMenuId = 0;
	g_hasReservedMenu = false;
	g_menuUnavailableLogged = false;

	g_commands.clear();
	g_listeners.clear();
	g_hookedEvents.clear();

	g_mapReady = false;
	g_mapEndSeen = false;
	g_mapListeners = false;
	g_mapStartAttempted = false;
	g_mapStartRegistered = false;
	g_mapEndAttempted = false;
	g_mapEndRegistered = false;
	g_firstUpdateDone = false;
	g_started = false;
	g_needReconcile = false;
	g_shuttingDown = false;

	g_refreshAccumulator = 0.0;
	g_tagAccumulator = 0.0;
	g_inviteAccumulator = 0.0;

	g_warmupOffset = -1;
	g_clanOffset = -1;
	g_clanChainOffset = -1;
	g_teamOffset = -1;
}

plg::PluginResult PluginStartImpl() {
	try {
		ResetRuntimeState();
		LoadConfiguration();
		LoadPhrases();
		EnsureDatabase();

		RegisterMapListeners();
		RegisterEvents();
		RegisterCommands();

		// The dummy menu burns s2sdk's MenuId 0 before any real menu can be created; it is only
		// made when every teardown binding is present.
		if (!MenuBindingsReady()) {
			LogMenuBindingsUnavailableOnce();
		} else if (!ReserveMenuIdZero()) {
			LogError(std::string("[") + kLogTag +
					 "] could not reserve s2sdk menu id 0 - the clan menu is disabled");
		}

		g_started = true;
		LogInfo(std::string("[") + kLogTag + "] loaded (in-game runtime; the site owns clans).");
	} catch (const std::exception& exception) {
		LogError(std::string("[") + kLogTag + "] OnPluginStart failed: " + exception.what());
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] OnPluginStart failed with an unknown exception");
	}
	// Never Failed: that stops every Plugify plugin (PITFALLS A1).
	return {};
}

plg::PluginResult PluginUpdateImpl(std::chrono::milliseconds dt) {
	if (!g_started) {
		return {};
	}
	try {
		PollMapGate();

		// Deferred work first: the menu handler only queues, so the menu manager's lock is never
		// re-entered and no s2sdk timer mutex is involved (this plugin owns no timers).
		DrainMenuDestroys();
		RetryParkedMenus();
		DrainActions();
		if (g_database) {
			g_database->Poll();
		}

		const double elapsed = dt.count() > 0 ? static_cast<double>(dt.count()) / 1000.0 : 0.0;
		// Pure in-memory expiry: no client/entity call, so it may run with the gate shut.
		g_inviteAccumulator += elapsed;
		if (g_inviteAccumulator >= 1.0) {
			g_inviteAccumulator = 0.0;
			g_invites.Cleanup(static_cast<int64_t>(std::time(nullptr)));
		}

		if (!g_mapReady) {
			return {};
		}
		if (g_needReconcile) {
			g_needReconcile = false;
			ReconcileAll(true);
		}
		if (g_config.membershipRefreshSeconds > 0) {
			g_refreshAccumulator += elapsed;
			if (g_refreshAccumulator >= static_cast<double>(g_config.membershipRefreshSeconds)) {
				g_refreshAccumulator = 0.0;
				ReconcileAll(true);
			}
		}
		if (g_config.showTagOnScoreboard && g_config.scoreboardRefreshSeconds > 0) {
			g_tagAccumulator += elapsed;
			if (g_tagAccumulator >= static_cast<double>(g_config.scoreboardRefreshSeconds)) {
				g_tagAccumulator = 0.0;
				for (const auto& [steamid, info] : g_players) {
					if (info.loaded && info.clanId > 0 && SlotStillSteam(info.slot, steamid)) {
						ApplyScoreboardTag(info.slot, FormatClanTag(info.clanTag));
					}
				}
			}
		}
	} catch (const std::exception& exception) {
		LogError(std::string("[") + kLogTag + "] OnPluginUpdate failed: " + exception.what());
	} catch (...) {
		LogError(std::string("[") + kLogTag + "] OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult PluginEndImpl() {
	try {
		g_started = false;
		g_shuttingDown = true;

		// Menus first: their handler belongs to this library. On a live map a backend close is
		// safe; between maps the checked teardown switches to the builtin no-op-close type and
		// reports A7_UNRESOLVED if s2sdk refuses to release an id.
		TeardownMenusOnUnload();
		RemoveEvents();
		RemoveCommands();
		UnregisterMapListeners();

		g_players.clear();
		g_slotSteam.fill(0);
		g_store.reset();
		g_database.reset();
		ResetRuntimeState();
	} catch (...) {
	}
	return {};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------------------------

class GuildsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override { return PluginStartImpl(); }
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		return PluginUpdateImpl(dt);
	}
	plg::PluginResult OnPluginEnd() override { return PluginEndImpl(); }
};

GuildsPlugin g_guildsPlugin;

} // namespace k4

PLUGIFY_PLUGIN(PLUGIN_API, &k4::g_guildsPlugin)
