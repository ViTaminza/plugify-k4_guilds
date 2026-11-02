// k4_guilds plugin tests — no engine: src/plugin.cpp is compiled into this TU (its state and
// handlers live in k4's internal namespace) and every __s2sdk_* / __hudmenu_* binding slot is a
// recording fake. The database is a real SQLite file: the plugin's plgdb worker and its queries
// run for real, so the menu/action flows are exercised end to end.
//
// Covered:
//   * startup (PITFALLS A1): OnPluginStart is Ok even without a config/database/bindings, every
//     registration happens once (map listeners, five game events as Pre hooks, the console/chat
//     names and the say listeners), the dummy MenuId 0 is reserved before any real menu, and no
//     client/entity/schema call is made before the first map (B1);
//   * the map gate (B1/B2): no client call in OnPluginStart, between OnMapEnd and OnMapStart, or
//     while GetCurrentMap() reports "" / "<empty>"; a plugin loaded into a running map latches
//     readiness on the FIRST update, and an OnMapEnd seen before that keeps it shut;
//   * C1: the scoreboard tag is written through a handle that may read as a negative int; 0 and -1
//     are refused, IsValidEntHandle(false) is refused, and no `handle <= 0` test exists;
//   * C2: bots, SourceTV and disconnected slots are skipped by every client path;
//   * C8: schema offsets are only resolved with the gate open and only successful ones are cached;
//   * C9/B4: the plugin creates no s2sdk timer; the refresh/tag/invite periods are accumulators;
//   * C10: menus are native; a selection is only queued inside the handler (no DB/menu work
//     there), the queued action carries the SteamID64 + controller handle and is dropped when the
//     slot is reused; a changelevel ends the display with CancelClientMenu(Disconnect), re-checks
//     GetClientMenu, switches a surviving display to the no-op-close "chat" type and destroys it;
//     a menu s2sdk refuses to destroy is parked and finally reported as A7_UNRESOLVED;
//   * C11: server console is strictly caller == -1, client console/chat 0..63, any other caller is
//     refused; the "." alias is caught by the say listener; command ownership (RemoveCommand vs
//     RemoveCommandListener) decides the teardown;
//   * C12: "userid"/"attacker" are read with GetEventPlayerSlot, GetEventInt is never called, and
//     every event hook answers Continue;
//   * kill rewards: warmup, bots, same-team/same-clan rules and the bank/kills/deaths writes reach
//     the database;
//   * the real action flows on SQLite: open-clan join, invite prompt + accept, kick;
//   * config/phrase priority (<configs>/k4_guilds/... first, core.cfg ServerLang -> s2sdk -> en)
//     and the databases.cfg fail-soft SQLite fallback;
//   * teardown (A7): OnPluginEnd removes every command/listener/event/map listener and every menu,
//     and a reload registers everything exactly once again.
//
// Run by ./test.sh (build/tests/k4_guilds_plugin_tests).

#include "../src/plugin.cpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using k4::ToPlg;
using k4::ToStd;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	Check(actual == expected,
		  what + " (got " + std::to_string(actual) + ", want " + std::to_string(expected) + ")");
}

void CheckEqStr(const std::string& actual, const std::string& expected, const std::string& what) {
	Check(actual == expected, what + " (got \"" + actual + "\", want \"" + expected + "\")");
}

void Section(const char* name) { std::printf("== %s\n", name); }

// ==========================================================================================
// filesystem fixture
// ==========================================================================================

std::string g_baseDir;
std::string g_configsDir;
std::string g_dataDir;

std::string ReadTextFile(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) return {};
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

void WriteTextFile(const std::string& path, std::string_view content) {
	const std::filesystem::path parent = std::filesystem::path(path).parent_path();
	if (!parent.empty()) {
		std::error_code error;
		std::filesystem::create_directories(parent, error);
	}
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << content;
}

std::string ConfigPath() { return g_configsDir + "/k4_guilds/config.json"; }
std::string PhrasesPath() { return g_configsDir + "/k4_guilds/translations/k4_guilds.phrases.txt"; }
std::string CoreConfigPath() { return g_configsDir + "/core.cfg"; }
std::string DatabasesPath() { return g_configsDir + "/k4_guilds/databases.cfg"; }
std::string SqlitePath() { return g_dataDir + "/k4_guilds/clans.sqlite3"; }

// ==========================================================================================
// fake clients
// ==========================================================================================

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool bot = false;
	int team = 0;
	uint64_t steam = 0;
	std::string name;
	bool hasPawn = true;
	int32_t controllerHandle = 0;
};

FakeClient g_clients[64];
int g_clientCalls = 0;
int g_entityCalls = 0;
int g_schemaCalls = 0;
int g_schemaOffsetLookups = 0;
int g_timerCalls = 0;
int g_maxClients = 64;
int g_team[64] = {};

void ResetCallCounters() {
	g_clientCalls = 0;
	g_entityCalls = 0;
	g_schemaCalls = 0;
	g_schemaOffsetLookups = 0;
	g_timerCalls = 0;
}

void* ControllerPtr(int slot) { return reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + slot)); }
void* PawnPtr(int slot) { return reinterpret_cast<void*>(static_cast<uintptr_t>(0x20000 + slot)); }

int32_t DefaultControllerHandle(int slot) {
	return static_cast<int32_t>(0x40000000u | static_cast<uint32_t>(slot));
}

void AddHuman(int slot, uint64_t steam, int team, const std::string& name) {
	FakeClient& client = g_clients[slot];
	client = FakeClient{};
	g_team[slot] = team;
	client.connected = true;
	client.inGame = true;
	client.bot = false;
	client.team = team;
	client.steam = steam;
	client.name = name;
	client.controllerHandle = DefaultControllerHandle(slot);
}

void AddBot(int slot, int team, const std::string& name) {
	FakeClient& client = g_clients[slot];
	client = FakeClient{};
	g_team[slot] = team;
	client.connected = true;
	client.inGame = true;
	client.bot = true;
	client.team = team;
	client.name = name;
	client.controllerHandle = DefaultControllerHandle(slot);
}

void RemoveClient(int slot) { g_clients[slot] = FakeClient{}; }

plg::string FakeGetClientName(int32_t slot) {
	if (slot < 0 || slot >= 64) return plg::string();
	return plg::string(g_clients[slot].name.c_str());
}

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	return slot >= 0 && slot < 64 && g_clients[slot].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	return slot >= 0 && slot < 64 && g_clients[slot].inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	return slot >= 0 && slot < 64 && g_clients[slot].sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_clientCalls;
	return slot >= 0 && slot < 64 && g_clients[slot].bot;
}

s2sdk::CSTeam FakeGetClientTeam(int32_t slot) {
	++g_clientCalls;
	if (slot < 0 || slot >= 64) return s2sdk::CSTeam::None;
	return static_cast<s2sdk::CSTeam>(g_clients[slot].team);
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientCalls;
	if (slot < 0 || slot >= 64) return 0;
	return g_clients[slot].steam;
}

void* FakeGetClientPawn(int32_t slot) {
	++g_clientCalls;
	if (slot < 0 || slot >= 64 || !g_clients[slot].hasPawn) return nullptr;
	return PawnPtr(slot);
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientCalls;
	if (slot < 0 || slot >= 64 || !g_clients[slot].connected) return nullptr;
	return ControllerPtr(slot);
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_clientCalls;
	if (slot < 0 || slot >= 64 || !g_clients[slot].connected) return -1;
	return g_clients[slot].controllerHandle;
}

int32_t FakeEntPointerToEntHandle(void* ptr) {
	++g_entityCalls;
	if (!ptr) return -1;
	const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
	if (value >= 0x20000 && value < 0x20000 + 64) {
		return static_cast<int32_t>(0xC0000000u | static_cast<uint32_t>(value - 0x20000));
	}
	if (value >= 0x10000 && value < 0x10000 + 64) {
		return DefaultControllerHandle(static_cast<int>(value - 0x10000));
	}
	return -1;
}

std::set<int32_t> g_invalidHandles;

bool FakeIsValidEntHandle(int32_t handle) {
	++g_entityCalls;
	// PITFALLS C1: only -1 is INVALID_EHANDLE_INDEX; 0 is the world and a negative value with the
	// high serial bit set is a valid CEntityHandle.
	if (g_invalidHandles.count(handle) != 0) return false;
	return handle != -1;
}

bool FakeIsValidEntPointer(void* ptr) {
	++g_entityCalls;
	return ptr != nullptr;
}

int32_t FakeGetMaxClients() {
	++g_clientCalls;
	return g_maxClients;
}

// ==========================================================================================
// fake map lifecycle
// ==========================================================================================

std::string g_mapName;
s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;
bool g_throwMapEndRegister = false;

plg::string FakeGetCurrentMap() { return plg::string(g_mapName.c_str()); }

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapStartCallback = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback) {
	g_mapStartCallback = nullptr;
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	if (g_throwMapEndRegister) throw std::runtime_error("fake OnMapEnd register failure");
	g_mapEndCallback = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback) {
	g_mapEndCallback = nullptr;
	++g_mapEndUnregisters;
}

// ==========================================================================================
// fake game events
// ==========================================================================================

struct FakeEvent {
	std::map<std::string, int32_t> slots;
	std::map<std::string, std::string> strings;
};

struct FakeEventHook {
	s2sdk::EventCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::map<std::string, FakeEventHook> g_eventHooks;
std::vector<std::string> g_eventSlotReads;
std::vector<std::string> g_eventIntReads;
int g_hookCalls = 0;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback,
									s2sdk::HookMode mode) {
	g_eventHooks[ToStd(name)] = FakeEventHook{callback, mode};
	++g_hookCalls;
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback,
									  s2sdk::HookMode mode) {
	const auto it = g_eventHooks.find(ToStd(name));
	if (it == g_eventHooks.end() || it->second.callback != callback || it->second.mode != mode) {
		return s2sdk::EventHookError::InvalidEvent;
	}
	g_eventHooks.erase(it);
	return s2sdk::EventHookError::Okay;
}

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	if (!event) return -1;
	g_eventSlotReads.push_back(ToStd(key));
	const auto* e = static_cast<const FakeEvent*>(event);
	const auto it = e->slots.find(ToStd(key));
	return it == e->slots.end() ? -1 : it->second;
}

int32_t FakeGetEventInt(void* /*event*/, const plg::string& key) {
	// A decoy: a player field read through GetEventInt would return the raw user id, not a slot.
	g_eventIntReads.push_back(ToStd(key));
	return 999;
}

s2sdk::ResultType FireGameEvent(const std::string& name, FakeEvent* event) {
	const auto it = g_eventHooks.find(name);
	if (it == g_eventHooks.end() || it->second.callback == nullptr) {
		Check(false, "event '" + name + "' is not hooked");
		return s2sdk::ResultType::Continue;
	}
	return it->second.callback(plg::string(name.c_str()), event, false);
}

// ==========================================================================================
// fake schema / game rules
// ==========================================================================================

void* const kRulesPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(0x30000));
int32_t g_warmupOffset = 100;
int32_t g_teamOffset = 110;
int32_t g_clanOffset = 120;
int32_t g_chainOffset = -1;
bool g_warmup = false;
std::vector<std::tuple<int32_t, int32_t, std::string, int32_t>> g_tagWrites; // handle, offset, text, chain

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& fieldName) {
	++g_schemaCalls;
	++g_schemaOffsetLookups;
	const std::string cls = ToStd(className);
	const std::string field = ToStd(fieldName);
	if (cls == "CCSPlayerController" && field == "m_szClan") return g_clanOffset;
	if (cls == "CCSGameRules" && field == "m_bWarmupPeriod") return g_warmupOffset;
	if (cls == "CBaseEntity" && field == "m_iTeamNum") return g_teamOffset;
	return -1;
}

int32_t FakeGetSchemaChainOffset(const plg::string&) {
	++g_schemaCalls;
	return g_chainOffset;
}

int64_t FakeGetEntData2(void* entity, int32_t offset, int32_t size) {
	++g_schemaCalls;
	if (entity == kRulesPtr && offset == g_warmupOffset && size == 1) return g_warmup ? 1 : 0;
	const uintptr_t value = reinterpret_cast<uintptr_t>(entity);
	if (value >= 0x10000 && value < 0x10000 + 64 && offset == g_teamOffset && size == 1) {
		return g_team[value - 0x10000];
	}
	return 0;
}

void* FakeGetGameRules() {
	++g_schemaCalls;
	return kRulesPtr;
}

void FakeSetEntDataCString(int32_t handle, int32_t offset, const plg::string& value, int32_t /*size*/,
						   bool, int32_t chainOffset) {
	++g_schemaCalls;
	g_tagWrites.emplace_back(handle, offset, ToStd(value), chainOffset);
}

// ==========================================================================================
// fake chat / logs
// ==========================================================================================

struct LogEntry {
	std::string text;
	plg::Severity severity = plg::Severity::Info;
};

std::vector<LogEntry> g_logs;
std::vector<std::pair<int32_t, std::string>> g_chat;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location&) {
	g_logs.push_back(LogEntry{std::string(message), severity});
}

bool LoggedWith(plg::Severity severity, const std::string& text) {
	for (const LogEntry& entry : g_logs) {
		if (entry.severity == severity && entry.text.find(text) != std::string::npos) return true;
	}
	return false;
}

void FakePrintToChat(int32_t slot, const plg::string& message) {
	g_chat.emplace_back(slot, ToStd(message));
}

plg::string FakeGetConfigsDir() { return plg::string(g_configsDir.c_str()); }
plg::string FakeGetDataDir() { return plg::string(g_dataDir.c_str()); }

plg::string g_sdkLanguage = "english";

plg::string FakeGetServerLanguage() { return plg::string(g_sdkLanguage.c_str()); }

// ==========================================================================================
// fake commands / listeners
// ==========================================================================================

struct FakeCommand {
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	bool listener = false;
};

std::map<std::string, FakeCommand> g_commands;
std::map<std::string, bool> g_commandCreated;
std::vector<std::string> g_removedCommands;
std::vector<std::string> g_removedListeners;
int g_addConsoleCommandCalls = 0;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string&, s2sdk::ConVarFlag flags,
						   s2sdk::ConCommandCallback callback, s2sdk::HookMode mode) {
	++g_addConsoleCommandCalls;
	g_commands[ToStd(name)] = FakeCommand{callback, mode, flags, false};
	const auto it = g_commandCreated.find(ToStd(name));
	return it == g_commandCreated.end() ? true : it->second;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback callback) {
	const auto it = g_commands.find(ToStd(name));
	if (it == g_commands.end() || it->second.callback != callback) return false;
	g_commands.erase(it);
	g_removedCommands.push_back(ToStd(name));
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode mode) {
	g_commands[ToStd(name)] = FakeCommand{callback, mode, s2sdk::ConVarFlag::None, true};
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							   s2sdk::HookMode) {
	const auto it = g_commands.find(ToStd(name));
	if (it == g_commands.end() || it->second.callback != callback) return false;
	g_commands.erase(it);
	g_removedListeners.push_back(ToStd(name));
	return true;
}

s2sdk::ResultType RunCommand(const std::string& name, int32_t caller,
							 s2sdk::ConCommandContext context, std::vector<std::string> args) {
	const auto it = g_commands.find(name);
	if (it == g_commands.end() || it->second.callback == nullptr) {
		Check(false, "command '" + name + "' is not registered");
		return s2sdk::ResultType::Continue;
	}
	plg::vector<plg::string> plgArgs;
	plgArgs.resize(args.size());
	for (std::size_t i = 0; i < args.size(); ++i) plgArgs[i] = ToPlg(args[i]);
	return it->second.callback(caller, context, plgArgs);
}

// ==========================================================================================
// fake menus
// ==========================================================================================

struct FakeMenuItem {
	std::string info;
	std::string display;
	s2sdk::MenuItemStyle style = s2sdk::MenuItemStyle::Default;
};

struct FakeMenu {
	std::string title;
	std::string type;
	s2sdk::MenuHandlerCallback handler = nullptr;
	std::vector<FakeMenuItem> items;
	bool exit = false;
	bool exitBack = false;
	bool closeOnSelect = true;
	int32_t pagination = 0;
};

std::map<s2sdk::MenuId, FakeMenu> g_menus;
std::map<int32_t, s2sdk::MenuId> g_clientMenus;
std::map<int32_t, int32_t> g_menuOffsets;
s2sdk::MenuId g_nextMenuId = 0;
std::vector<s2sdk::MenuId> g_createdMenus;
std::vector<s2sdk::MenuId> g_destroyedMenus;
std::set<std::string> g_registeredTypes = {"chat", "console", "centerhtml", "button", "hud", "wasd"};
std::map<int32_t, std::string> g_menuChoices;
int g_hudmenuCalls = 0;
bool g_cancelFails = false;
bool g_cancelLeavesState = false;
bool g_setMenuTypeFails = false;
bool g_destroyFails = false;
bool g_displayFails = false;
int g_backendCloses = 0;

const FakeMenu* MenuOfSlot(int32_t slot) {
	const auto client = g_clientMenus.find(slot);
	if (client == g_clientMenus.end()) return nullptr;
	const auto menu = g_menus.find(client->second);
	return menu == g_menus.end() ? nullptr : &menu->second;
}

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler,
							 const plg::string& type) {
	const s2sdk::MenuId id = g_nextMenuId++;
	FakeMenu menu;
	menu.title = ToStd(title);
	menu.type = ToStd(type);
	menu.handler = handler;
	g_menus[id] = std::move(menu);
	g_createdMenus.push_back(id);
	return id;
}

void CallHandler(const FakeMenu& menu, s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot,
				 int32_t param) {
	if (menu.handler != nullptr) menu.handler(id, action, slot, param);
}

bool FakeDestroyMenu(s2sdk::MenuId id) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) return false;
	if (g_destroyFails) return false; // s2sdk refused to release the id
	for (auto client = g_clientMenus.begin(); client != g_clientMenus.end();) {
		if (client->second == id) {
			if (it->second.type != "chat" && it->second.type != "console") ++g_backendCloses;
			CallHandler(it->second, id, s2sdk::MenuAction::Cancel, client->first,
						static_cast<int32_t>(s2sdk::MenuCancelReason::Destroyed));
			CallHandler(it->second, id, s2sdk::MenuAction::End, client->first, 0);
			client = g_clientMenus.erase(client);
		} else {
			++client;
		}
	}
	g_menus.erase(it);
	g_destroyedMenus.push_back(id);
	return true;
}

bool FakeIsValidMenu(s2sdk::MenuId id) { return g_menus.count(id) != 0; }

int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle style) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) return -1;
	it->second.items.push_back(FakeMenuItem{ToStd(info), ToStd(display), style});
	return static_cast<int32_t>(it->second.items.size()) - 1;
}

bool FakeSetMenuPagination(s2sdk::MenuId id, int32_t itemsPerPage) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) return false;
	it->second.pagination = itemsPerPage;
	return true;
}

bool FakeSetMenuExitButton(s2sdk::MenuId id, bool enabled) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) return false;
	it->second.exit = enabled;
	return true;
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool enabled) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) return false;
	it->second.exitBack = enabled;
	return true;
}

bool FakeSetMenuCloseOnSelect(s2sdk::MenuId id, bool enabled) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) return false;
	it->second.closeOnSelect = enabled;
	return true;
}

bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end() || g_displayFails) return false;
	const auto current = g_clientMenus.find(slot);
	if (current != g_clientMenus.end()) {
		const auto old = g_menus.find(current->second);
		if (old != g_menus.end()) {
			CallHandler(old->second, current->second, s2sdk::MenuAction::Cancel, slot,
						static_cast<int32_t>(s2sdk::MenuCancelReason::Interrupted));
			CallHandler(old->second, current->second, s2sdk::MenuAction::End, slot, 0);
		}
		g_clientMenus.erase(current);
	}
	g_clientMenus[slot] = id;
	g_menuOffsets[slot] = 0;
	CallHandler(it->second, id, s2sdk::MenuAction::Start, slot, 0);
	return true;
}

s2sdk::MenuId FakeGetClientMenu(int32_t slot) {
	const auto it = g_clientMenus.find(slot);
	return it == g_clientMenus.end() ? 0 : it->second;
}

int32_t FakeGetClientMenuOffset(int32_t slot) {
	const auto it = g_menuOffsets.find(slot);
	return it == g_menuOffsets.end() ? 0 : it->second;
}

bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	if (g_cancelFails) return false;
	if (g_cancelLeavesState) return true;
	const auto it = g_clientMenus.find(slot);
	if (it == g_clientMenus.end()) return false;
	const s2sdk::MenuId id = it->second;
	g_clientMenus.erase(it);
	const auto menu = g_menus.find(id);
	if (menu != g_menus.end()) {
		CallHandler(menu->second, id, s2sdk::MenuAction::Cancel, slot,
					static_cast<int32_t>(reason));
		CallHandler(menu->second, id, s2sdk::MenuAction::End, slot, 0);
	}
	return true;
}

bool FakeIsMenuTypeRegistered(const plg::string& name) {
	return g_registeredTypes.count(ToStd(name)) != 0;
}

bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& type) {
	if (g_setMenuTypeFails) return false;
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) return false;
	it->second.type = ToStd(type);
	return true;
}

plg::string FakeGetMenuType(s2sdk::MenuId id) {
	const auto it = g_menus.find(id);
	return it == g_menus.end() ? plg::string() : plg::string(it->second.type.c_str());
}

plg::string FakeGetClientMenuType(int32_t slot) {
	++g_hudmenuCalls;
	const auto it = g_menuChoices.find(slot);
	return it == g_menuChoices.end() ? plg::string() : plg::string(it->second.c_str());
}

s2sdk::TimerId FakeCreateTimer(double, s2sdk::TimerCallback, s2sdk::TimerFlag,
							   const plg::vector<plg::any>&) {
	++g_timerCalls;
	return 1;
}

// ==========================================================================================
// installation / reset
// ==========================================================================================

bool g_pluginStarted = false;

void InstallFakes() {
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientTeam = &FakeGetClientTeam;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_GetMaxClients = &FakeGetMaxClients;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;

	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	__s2sdk_GetServerLanguage = &FakeGetServerLanguage;
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventInt = &FakeGetEventInt;

	__s2sdk_PrintToChat = &FakePrintToChat;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_IsValidMenu = &FakeIsValidMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_GetClientMenuOffset = &FakeGetClientMenuOffset;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_SetMenuCloseOnSelect = &FakeSetMenuCloseOnSelect;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__hudmenu_GetClientMenuType = &FakeGetClientMenuType;

	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetSchemaChainOffset = &FakeGetSchemaChainOffset;
	__s2sdk_GetEntData2 = &FakeGetEntData2;
	__s2sdk_GetGameRules = &FakeGetGameRules;
	__s2sdk_SetEntDataCString = &FakeSetEntDataCString;

	__s2sdk_CreateTimer = &FakeCreateTimer;
}

// SQLite schema of the live clans tables (the same DDL the store tests use).
bool CreateSchema(plgdb::Database& database) {
	const std::vector<std::string> statements = {
		"CREATE TABLE clans_main (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, "
		"tag TEXT NOT NULL, `desc` TEXT NOT NULL DEFAULT '', owner TEXT NOT NULL, "
		"bank INTEGER NOT NULL DEFAULT 0, slots INTEGER NOT NULL DEFAULT 0, "
		"type INTEGER DEFAULT 1, exp INTEGER NOT NULL DEFAULT 0, "
		"role_count INTEGER NOT NULL DEFAULT 0, created INTEGER NOT NULL DEFAULT 0, "
		"lastupdate INTEGER NOT NULL DEFAULT 0, duration INTEGER NOT NULL DEFAULT 0)",
		"CREATE TABLE clans_players (id INTEGER PRIMARY KEY AUTOINCREMENT, auth TEXT NOT NULL, "
		"name TEXT NOT NULL DEFAULT 'unknown', clan_id INTEGER NOT NULL, "
		"kills INTEGER NOT NULL DEFAULT 0, deaths INTEGER NOT NULL DEFAULT 0, "
		"joined INTEGER NOT NULL DEFAULT 0, lastactivity INTEGER NOT NULL DEFAULT 0, "
		"role_id INTEGER, bank INTEGER NOT NULL DEFAULT 0)",
		"CREATE TABLE clans_roles (id INTEGER PRIMARY KEY AUTOINCREMENT, clan_id INTEGER NOT NULL, "
		"name TEXT NOT NULL, type INTEGER NOT NULL DEFAULT 3, kick INTEGER NOT NULL DEFAULT 0, "
		"roles INTEGER NOT NULL DEFAULT 0, invite INTEGER NOT NULL DEFAULT 0, "
		"settings INTEGER NOT NULL DEFAULT 0)",
		"CREATE TABLE clans_upgrade (clan_id INTEGER PRIMARY KEY, slots_up INTEGER NOT NULL DEFAULT 0, "
		"roles_up INTEGER NOT NULL DEFAULT 0)",
		"CREATE TABLE clans_request (id INTEGER PRIMARY KEY AUTOINCREMENT, clan_id INTEGER NOT NULL, "
		"steamid TEXT NOT NULL, name TEXT NOT NULL DEFAULT '', date INTEGER NOT NULL DEFAULT 0)",
		"CREATE TABLE clans_logs (id INTEGER PRIMARY KEY AUTOINCREMENT, clan_id INTEGER NOT NULL, "
		"action TEXT NOT NULL, value TEXT NOT NULL, created INTEGER NOT NULL, status INTEGER NOT NULL)",
		"CREATE TABLE clans_reactions (id INTEGER PRIMARY KEY AUTOINCREMENT, clan_id INTEGER NOT NULL, "
		"auth TEXT NOT NULL, type INTEGER NOT NULL)",
		"CREATE TABLE clans_views (id INTEGER PRIMARY KEY AUTOINCREMENT, clan_id INTEGER NOT NULL, "
		"auth TEXT NOT NULL, created INTEGER NOT NULL)",
	};
	for (const std::string& statement : statements) {
		const plgdb::Result result = database.QuerySync(statement);
		if (!result.Ok()) {
			std::printf("schema error: %s\n", result.Error().c_str());
			return false;
		}
	}
	return true;
}

void ResetWorld() {
	std::error_code error;
	if (!g_baseDir.empty()) std::filesystem::remove_all(g_baseDir, error);
	g_baseDir = "/tmp/k4_guilds_plugin_" + std::to_string(::getpid());
	g_configsDir = g_baseDir + "/configs";
	g_dataDir = g_baseDir + "/data";
	std::filesystem::create_directories(g_configsDir + "/k4_guilds/translations", error);
	std::filesystem::create_directories(g_dataDir + "/k4_guilds", error);

	WriteTextFile(ConfigPath(), ReadTextFile(std::string(K4_GUILDS_TEST_SOURCE_DIR) + "/assets/config.json"));
	WriteTextFile(PhrasesPath(),
				  ReadTextFile(std::string(K4_GUILDS_TEST_SOURCE_DIR) +
							   "/translations/k4_guilds.phrases.txt"));
	WriteTextFile(CoreConfigPath(), "\"Core\"\n{\n    \"ServerLang\" \"english\"\n}\n");
	WriteTextFile(DatabasesPath(),
				  "\"Databases\"\n{\n    \"k4_guilds\"\n    {\n"
				  "        \"driver\" \"sqlite\"\n"
				  "        \"sqlite_path\" \"clans.sqlite3\"\n"
				  "    }\n}\n");

	// Create the clans schema in the file the plugin will open (lazily) after StartPlugin.
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = SqlitePath();
	std::unique_ptr<plgdb::Database> seed = plgdb::Database::Create(config);
	Check(seed != nullptr, "seed database created");
	if (seed) Check(CreateSchema(*seed), "clans schema created");
	seed.reset();

	for (FakeClient& client : g_clients) client = FakeClient{};
	g_logs.clear();
	g_chat.clear();
	g_eventHooks.clear();
	g_eventSlotReads.clear();
	g_eventIntReads.clear();
	g_hookCalls = 0;
	g_commands.clear();
	g_commandCreated.clear();
	g_removedCommands.clear();
	g_removedListeners.clear();
	g_addConsoleCommandCalls = 0;
	g_menus.clear();
	g_clientMenus.clear();
	g_menuOffsets.clear();
	g_nextMenuId = 0;
	g_createdMenus.clear();
	g_destroyedMenus.clear();
	g_registeredTypes = {"chat", "console", "centerhtml", "button", "hud", "wasd"};
	g_menuChoices.clear();
	g_hudmenuCalls = 0;
	g_cancelFails = false;
	g_cancelLeavesState = false;
	g_setMenuTypeFails = false;
	g_destroyFails = false;
	g_displayFails = false;
	g_backendCloses = 0;
	g_invalidHandles.clear();
	g_tagWrites.clear();
	g_mapName.clear();
	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;
	g_throwMapEndRegister = false;
	g_warmup = false;
	g_warmupOffset = 100;
	g_teamOffset = 110;
	g_clanOffset = 120;
	g_chainOffset = -1;
	for (int& team : g_team) team = 0;
	g_maxClients = 64;
	g_sdkLanguage = "english";
	ResetCallCounters();
}

void PrepareTest() {
	if (g_pluginStarted) {
		k4::g_guildsPlugin.OnPluginEnd();
		g_pluginStarted = false;
	}
	ResetWorld();
	InstallFakes();
}

void StartPlugin() {
	const plg::PluginResult result = k4::g_guildsPlugin.OnPluginStart();
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart is Ok");
	g_pluginStarted = true;
}

void StopPlugin() {
	const plg::PluginResult result = k4::g_guildsPlugin.OnPluginEnd();
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginEnd is Ok");
	g_pluginStarted = false;
}

void Update(int milliseconds = 100) {
	const plg::PluginResult result =
		k4::g_guildsPlugin.OnPluginUpdate(std::chrono::milliseconds(milliseconds));
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginUpdate is Ok");
}

bool DbIdle() { return k4::g_database == nullptr || k4::g_database->Pending() == 0; }

// Runs frames until the plgdb worker and the follow-up callbacks are quiet.
void Settle(int frames = 2000) {
	// The plgdb worker is a real thread: every frame below must also give it real wall-clock time,
	// and the completion that arrives during that wait has to be polled before "idle" is decided.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	int idle = 0;
	for (int i = 0; i < frames && idle < 5 && std::chrono::steady_clock::now() < deadline; ++i) {
		Update(50);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		const std::size_t ran = k4::g_database ? k4::g_database->Poll() : 0;
		if (DbIdle() && ran == 0) {
			++idle;
		} else {
			idle = 0;
		}
	}
	// Late completions: drain whatever the worker finished between the last poll and the last check.
	for (int i = 0; i < 5; ++i) {
		Update(50);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		if (k4::g_database) k4::g_database->Poll();
	}
}
void FireMapStart() {
	if (g_mapStartCallback == nullptr) {
		Check(false, "the OnMapStart listener is not registered");
		return;
	}
	g_mapStartCallback();
}

void FireMapEnd() {
	if (g_mapEndCallback == nullptr) {
		Check(false, "the OnMapEnd listener is not registered");
		return;
	}
	g_mapEndCallback();
}

void OpenMap() {
	g_mapName = "de_dust2";
	FireMapStart();
	Update();
}

void SeedClan(int type = 1, bool withMember = true, int32_t bank = 5) {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = SqlitePath();
	std::unique_ptr<plgdb::Database> seed = plgdb::Database::Create(config);
	Check(seed != nullptr, "seed database for clan");
	if (!seed) return;
	seed->QuerySync(
		"INSERT INTO clans_main (id, name, tag, `desc`, owner, bank, slots, type, exp, role_count, "
		"created, lastupdate, duration) VALUES "
		"(1, 'Test Clan', 'TST', 'desc', '76561198000000001', ?, 10, ?, 1000, 3, 100, 200, 0)",
		{static_cast<int64_t>(bank), static_cast<int64_t>(type)});
	seed->QuerySync("INSERT INTO clans_upgrade (clan_id, slots_up, roles_up) VALUES (1, 0, 0)");
	seed->QuerySync(
		"INSERT INTO clans_roles (id, clan_id, name, type, kick, roles, invite, settings) VALUES "
		"(10, 1, 'Owner', 1, 1, 1, 1, 1)");
	seed->QuerySync(
		"INSERT INTO clans_roles (id, clan_id, name, type, kick, roles, invite, settings) VALUES "
		"(11, 1, 'Member', 2, 0, 0, 1, 0)");
	seed->QuerySync(
		"INSERT INTO clans_players (auth, name, clan_id, kills, deaths, joined, lastactivity, role_id) "
		"VALUES ('76561198000000001', 'Owner', 1, 10, 5, 1000, 1000, 10)");
	if (withMember) {
		seed->QuerySync(
			"INSERT INTO clans_players (auth, name, clan_id, kills, deaths, joined, lastactivity, "
			"role_id) VALUES ('76561198000000002', 'Member', 1, 4, 9, 1001, 1001, 11)");
	}
	seed.reset();
}

int64_t QueryInt(const std::string& sql) {
	if (!k4::g_database) return -1;
	const plgdb::Result result = k4::g_database->QuerySync(sql);
	if (!result.Ok() || result.RowCount() == 0 || result.ColumnCount() == 0) return -1;
	return result.GetInt(0, 0, -1);
}

FakeMenu* MenuOf(int32_t slot) { return const_cast<FakeMenu*>(MenuOfSlot(slot)); }

int32_t ItemIndex(const FakeMenu& menu, const std::string& info) {
	for (std::size_t i = 0; i < menu.items.size(); ++i) {
		if (menu.items[i].info == info) return static_cast<int32_t>(i);
	}
	return -1;
}

// Simulates s2sdk's Select without closeOnSelect: the display stays, the handler queues the action.
bool SelectMenu(int32_t slot, const std::string& info) {
	FakeMenu* menu = MenuOf(slot);
	if (menu == nullptr || menu->handler == nullptr) return false;
	const int32_t index = ItemIndex(*menu, info);
	if (index < 0) return false;
	// The one-per-second selection throttle is not what a test is about; release it.
	k4::g_lastMenuActionAt[static_cast<std::size_t>(slot)] = std::chrono::steady_clock::time_point{};
	menu->handler(g_clientMenus[slot], s2sdk::MenuAction::Select, slot, index);
	return true;
}

// ==========================================================================================
// tests
// ==========================================================================================

void TestStartupAndMapGate() {
	Section("startup (A1) and the map gate (B1/B2)");
	PrepareTest();
	StartPlugin();
	CheckEqInt(g_mapStartRegisters, 1, "OnMapStart registered once");
	CheckEqInt(g_mapEndRegisters, 1, "OnMapEnd registered once");
	CheckEqInt(g_hookCalls, 5, "five game events hooked");
	bool allPre = true;
	for (const auto& [name, hook] : g_eventHooks) {
		if (hook.mode != s2sdk::HookMode::Pre) allPre = false;
	}
	Check(allPre, "every game event is a Pre hook");
	Check(g_eventHooks.count("player_activate") == 1 && g_eventHooks.count("player_disconnect") == 1 &&
			  g_eventHooks.count("player_death") == 1 && g_eventHooks.count("player_spawn") == 1 &&
			  g_eventHooks.count("player_team") == 1,
		  "the five production events are hooked");
	Check(g_commands.count("clans") == 1 && g_commands.count("clan_menu") == 1 &&
			  g_commands.count("clan") == 1 && g_commands.count("gc") == 1,
		  "console + chat aliases and gc are registered");
	Check(g_commands.count("say") == 1 && g_commands.count("say_team") == 1,
		  "the '.' alias say listeners are registered");
	Check(g_commands["clans"].flags == static_cast<s2sdk::ConVarFlag>(
											static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
											static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute)),
		  "commands are client-executable and released");
	CheckEqInt(static_cast<int64_t>(g_createdMenus.size()), 1, "one dummy menu created at start");
	Check(!g_createdMenus.empty() && g_createdMenus.front() == 0, "dummy menu reserved MenuId 0");
	Check(!k4::g_mapReady, "the gate is shut at start");
	CheckEqInt(g_clientCalls, 0, "no client call in OnPluginStart");
	CheckEqInt(g_entityCalls, 0, "no entity call in OnPluginStart");
	CheckEqInt(g_schemaCalls, 0, "no schema call in OnPluginStart");
	CheckEqInt(g_timerCalls, 0, "the plugin creates no s2sdk timer");
	CheckEqInt(static_cast<int64_t>(g_menus.size()), 1, "only the reserved menu exists");

	// No map: the first update consumes the one-shot latch but keeps the gate shut.
	ResetCallCounters();
	Update();
	Check(!k4::g_mapReady, "GetCurrentMap() == \"\" keeps the gate shut");
	CheckEqInt(g_clientCalls, 0, "no client call before the first map");
	// "<empty>" is not a map either, and the latch is one-shot.
	g_mapName = "<empty>";
	Update();
	Check(!k4::g_mapReady, "\"<empty>\" keeps the gate shut");
	g_mapName = "de_dust2";
	Update();
	Check(!k4::g_mapReady, "the mid-map latch is decided on the first update only");
	// A real OnMapStart opens it, with no client/entity/schema call in the listener itself.
	ResetCallCounters();
	FireMapStart();
	Check(k4::g_mapReady, "OnMapStart opens the gate");
	CheckEqInt(g_clientCalls, 0, "OnMapStart only changes state");
	CheckEqInt(g_schemaCalls, 0, "OnMapStart resolves no schema offset");

	// OnMapEnd closes it and touches no client.
	ResetCallCounters();
	FireMapEnd();
	Check(!k4::g_mapReady, "OnMapEnd closes the gate");
	CheckEqInt(g_clientCalls, 0, "OnMapEnd only clears state (PITFALLS B2)");
	Update();
	Check(!k4::g_mapReady, "updates between maps do not reopen the gate");
	CheckEqInt(g_clientCalls, 0, "no client query between maps");
	FireMapStart();
	Check(k4::g_mapReady, "the next OnMapStart reopens the gate");
	StopPlugin();

	// A plugin loaded into a running map latches on its first update.
	PrepareTest();
	g_mapName = "de_dust2";
	StartPlugin();
	Check(!k4::g_mapReady, "a mid-map load starts with the gate shut");
	Update();
	Check(k4::g_mapReady, "the first update latches a running map");

	// If an OnMapEnd was seen first, the first update must not latch.
	PrepareTest();
	StartPlugin();
	FireMapEnd();
	Update();
	Check(!k4::g_mapReady, "OnMapEnd before the first update keeps the gate shut");
	Update();
	Check(!k4::g_mapReady, "later updates do not latch either");
	StopPlugin();

	// A failing OnMapEnd registration removes the half-registered pair and keeps the gate shut.
	PrepareTest();
	g_throwMapEndRegister = true;
	StartPlugin();
	CheckEqInt(g_mapStartRegisters, 1, "the start listener was attempted");
	CheckEqInt(g_mapEndRegisters, 0, "the end listener threw");
	CheckEqInt(g_mapStartUnregisters, 1, "the half-registered start listener was removed");
	g_mapName = "de_dust2";
	Update();
	Check(!k4::g_mapReady, "without the full pair the gate never opens");
	StopPlugin();

	// Missing bindings: fail soft, never Failed.
	PrepareTest();
	__s2sdk_OnMapEnd_Unregister = nullptr;
	StartPlugin();
	CheckEqInt(g_mapStartRegisters, 0, "no listener registered without the full pair");
	__s2sdk_HookEvent = nullptr;
	__s2sdk_AddConsoleCommand = nullptr;
	StopPlugin();
	PrepareTest();
	__s2sdk_HookEvent = nullptr;
	StartPlugin();
	Check(g_eventHooks.empty(), "no event is hooked without the pair");
	Check(!LoggedWith(plg::Severity::Error, "OnPluginStart failed"), "missing bindings are not fatal");
	StopPlugin();
}

void TestHandleAndSchemaGuards() {
	Section("C1 handle rule, C2 client checks, C8 schema caching");
	PrepareTest();
	SeedClan();
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	AddBot(2, 3, "Bot");
	AddHuman(3, 76561198000000003ULL, 2, "TV");
	g_clients[3].sourceTV = true;
	StartPlugin();
	// No map: ApplyScoreboardTag must not resolve a schema offset or touch a client.
	ResetCallCounters();
	k4::ApplyScoreboardTag(1, "TST");
	CheckEqInt(g_schemaCalls, 0, "no schema call with the gate shut");
	CheckEqInt(g_clientCalls, 0, "no client call with the gate shut");
	OpenMap();
	Settle();
	g_tagWrites.clear();

	// A negative controller handle (the serial counter passed bit 31) is valid.
	g_clients[1].controllerHandle = static_cast<int32_t>(0x80000001u);
	k4::ApplyScoreboardTag(1, "NEG");
	CheckEqInt(static_cast<int64_t>(g_tagWrites.size()), 1, "a negative handle writes the tag");
	if (!g_tagWrites.empty()) {
		CheckEqInt(std::get<0>(g_tagWrites.back()), static_cast<int64_t>(0x80000001u) - 0x100000000LL,
				   "the tag was written through the negative handle");
		CheckEqStr(std::get<2>(g_tagWrites.back()), "NEG", "the tag text is the formatted clan tag");
		CheckEqInt(std::get<3>(g_tagWrites.back()), -1, "no chain offset for the controller");
	}

	// 0 (world) and -1 (invalid) are refused by the handle rule; IsValidEntHandle(false) too.
	g_tagWrites.clear();
	g_clients[1].controllerHandle = 0;
	k4::ApplyScoreboardTag(1, "ZERO");
	g_clients[1].controllerHandle = -1;
	k4::ApplyScoreboardTag(1, "INVALID");
	g_clients[1].controllerHandle = DefaultControllerHandle(1);
	g_invalidHandles.insert(g_clients[1].controllerHandle);
	k4::ApplyScoreboardTag(1, "REJECTED");
	CheckEqInt(static_cast<int64_t>(g_tagWrites.size()), 0,
			   "0, -1 and an IsValidEntHandle(false) handle write nothing");
	g_invalidHandles.clear();

	// C2: bots, SourceTV and disconnected slots are never written.
	g_clients[1].controllerHandle = DefaultControllerHandle(1);
	k4::ApplyScoreboardTag(2, "BOT");
	k4::ApplyScoreboardTag(3, "TV");
	RemoveClient(4);
	k4::ApplyScoreboardTag(4, "GONE");
	CheckEqInt(static_cast<int64_t>(g_tagWrites.size()), 0, "bot/SourceTV/disconnected slots are skipped");

	// C8: the offset is resolved once and cached only after a successful lookup.
	g_schemaOffsetLookups = 0;
	k4::ApplyScoreboardTag(1, "A");
	k4::ApplyScoreboardTag(1, "B");
	CheckEqInt(g_schemaOffsetLookups, 0, "a cached offset is not resolved again");
	CheckEqInt(k4::g_clanOffset, g_clanOffset, "the successful offset is cached");
	// A chain offset > 0 is passed through; -1 (no chain) is normalised to -1.
	g_chainOffset = 40;
	CheckEqInt(k4::ResolveChainOffset("CCSPlayerController"), 40, "a positive chain offset is used");
	g_chainOffset = 0;
	CheckEqInt(k4::ResolveChainOffset("CCSPlayerController"), -1, "0 is normalised to -1");
	g_chainOffset = -1;

	// Team reads use the same gate-first offset cache (CBaseEntity::m_iTeamNum, 1 byte).
	CheckEqInt(k4::TeamOf(1), 2, "the controller team is read through the cached offset");
	CheckEqInt(k4::TeamOf(2), 3, "a bot controller still reports its team");
	StopPlugin();
}

void TestEventsAndKillRewards() {
	Section("events (C12) and kill rewards");
	PrepareTest();
	SeedClan(1, true, 5);
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	AddHuman(2, 76561198000000002ULL, 3, "Member");
	StartPlugin();
	OpenMap();
	Settle();

	ResetCallCounters();
	g_eventSlotReads.clear();
	g_eventIntReads.clear();
	FakeEvent death;
	death.slots["userid"] = 2;
	death.slots["attacker"] = 1;
	Check(FireGameEvent("player_death", &death) == s2sdk::ResultType::Continue,
		  "player_death answers Continue");
	Check(g_eventIntReads.empty(), "GetEventInt is never used for a player field");
	bool readsUserid = false;
	bool readsAttacker = false;
	for (const std::string& key : g_eventSlotReads) {
		if (key == "userid") readsUserid = true;
		if (key == "attacker") readsAttacker = true;
	}
	Check(readsUserid && readsAttacker, "userid/attacker are read with GetEventPlayerSlot");
	Settle();
	CheckEqInt(QueryInt("SELECT bank FROM clans_main WHERE id = 1"), 6,
			   "the same clan gets +3 for the kill and -2 for the death");
	CheckEqInt(QueryInt("SELECT kills FROM clans_players WHERE auth = '76561198000000001'"), 11,
			   "the killer's kills +1");
	CheckEqInt(QueryInt("SELECT deaths FROM clans_players WHERE auth = '76561198000000002'"), 10,
			   "the victim's deaths +1");

	// Warmup: ignore_warmup=true, no bank movement and no kill/death accounting.
	g_warmup = true;
	Check(FireGameEvent("player_death", &death) == s2sdk::ResultType::Continue,
		  "a warmup death answers Continue");
	Settle();
	CheckEqInt(QueryInt("SELECT bank FROM clans_main WHERE id = 1"), 6, "warmup pays no reward");
	CheckEqInt(QueryInt("SELECT kills FROM clans_players WHERE auth = '76561198000000001'"), 11,
			   "warmup counts no kill");
	g_warmup = false;

	// A death involving a bot changes nothing (count_bots=false).
	AddBot(3, 3, "Bot");
	FakeEvent botDeath;
	botDeath.slots["userid"] = 2;
	botDeath.slots["attacker"] = 3;
	FireGameEvent("player_death", &botDeath);
	Settle();
	CheckEqInt(QueryInt("SELECT bank FROM clans_main WHERE id = 1"), 6, "a bot death pays nothing");
	CheckEqInt(QueryInt("SELECT deaths FROM clans_players WHERE auth = '76561198000000002'"), 10,
			   "a bot death counts no death");

	// Between maps the event is acknowledged but touches nothing.
	FireMapEnd();
	ResetCallCounters();
	Check(FireGameEvent("player_death", &death) == s2sdk::ResultType::Continue,
		  "a death between maps answers Continue");
	CheckEqInt(g_clientCalls, 0, "no client call from an event between maps");
	StopPlugin();
}

void TestCommandsAndChat() {
	Section("commands (C11), the '.' alias and guild chat");
	PrepareTest();
	SeedClan(2, true);
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	AddHuman(2, 76561198000000002ULL, 3, "Member");
	StartPlugin();
	OpenMap();
	Settle();

	// Server console is strictly caller == -1 and only answers the text command set.
	g_logs.clear();
	Check(RunCommand("clans", -1, s2sdk::ConCommandContext::Console, {"clans"}) ==
			  s2sdk::ResultType::Handled,
		  "server-console clans is handled");
	Check(LoggedWith(plg::Severity::Info, "=== Clan Commands"), "server console gets the help text");
	// Any other out-of-range caller is refused without running anything.
	g_logs.clear();
	const std::size_t menusBefore = g_createdMenus.size();
	Check(RunCommand("clans", -2, s2sdk::ConCommandContext::Console, {"clans"}) ==
			  s2sdk::ResultType::Handled,
		  "caller -2 is refused (Handled)");
	Check(g_logs.empty(), "caller -2 runs nothing");
	CheckEqInt(static_cast<int64_t>(g_createdMenus.size()), static_cast<int64_t>(menusBefore),
			   "caller -2 opens no menu");
	// A chat-context call with an invalid caller is swallowed, never run as server console.
	g_logs.clear();
	RunCommand("clan", -5, s2sdk::ConCommandContext::Chat, {"clan"});
	Check(g_logs.empty(), "a malformed chat caller runs nothing");

	// Client console opens the clan menu.
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	Check(RunCommand("clan_menu", 1, s2sdk::ConCommandContext::Console, {"clan_menu"}) ==
			  s2sdk::ResultType::Handled,
		  "client-console clan_menu is handled");
	Settle();
	Check(MenuOf(1) != nullptr, "the client-console command opens the menu");

	// Chat "!clan" reaches the same trigger-less name.
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	Check(MenuOf(1) != nullptr, "chat !clan opens the menu");

	// The "." alias is caught by the say listener; a normal chat line is left alone.
	const std::size_t createdBefore = g_createdMenus.size();
	Check(RunCommand("say", 1, s2sdk::ConCommandContext::Console, {"say", "hello"}) ==
			  s2sdk::ResultType::Continue,
		  "a normal say line is not swallowed");
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	Check(RunCommand("say", 1, s2sdk::ConCommandContext::Console, {"say", ".clan"}) ==
			  s2sdk::ResultType::Handled,
		  "the '.' alias is swallowed");
	Settle();
	Check(g_createdMenus.size() > createdBefore, "the '.' alias opens the menu");

	// Guild chat: both members receive the line (runtime-only, no DB write).
	g_chat.clear();
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	Check(RunCommand("gc", 1, s2sdk::ConCommandContext::Console, {"gc", "hello", "clan"}) ==
			  s2sdk::ResultType::Handled,
		  "gc is handled");
	Settle();
	bool toMember = false;
	for (const auto& [slot, text] : g_chat) {
		if (slot == 2 && text.find("hello clan") != std::string::npos) toMember = true;
	}
	Check(toMember, "a clanmate receives the guild chat line");
	CheckEqInt(QueryInt("SELECT COUNT(*) FROM clans_logs"), 0, "guild chat writes no log row");

	// The one-per-second command throttle drops a repeated DB-backed subcommand.
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan", "info"});
	Settle();
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan", "info"});
	g_chat.clear();
	// the second call resets the stamp; an immediate third one is throttled
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan", "info"});
	Check(!g_chat.empty() && g_chat.back().second.find("wait") != std::string::npos,
		  "a throttled command explains itself");
	StopPlugin();
}

void TestNativeMenus() {
	Section("native menus (C10): queue, slot identity, teardown");
	PrepareTest();
	SeedClan(2, true);
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	AddHuman(2, 76561198000000002ULL, 3, "Member");
	StartPlugin();
	OpenMap();
	Settle();

	// pisexcompat ResolveS2sdkMenuTypeName table, through the real ResolveMenuType.
	g_menuChoices[1] = "chat";
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	Check(MenuOf(1) != nullptr && MenuOf(1)->type == "chat", "the player's 'chat' choice wins");
	g_menuChoices[1] = "wasd";
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	Check(MenuOf(1) != nullptr && MenuOf(1)->type == "wasd", "the 'wasd' choice uses the backend");
	g_registeredTypes.erase("wasd");
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	Check(MenuOf(1) != nullptr && MenuOf(1)->type == "button",
		  "a missing wasd backend falls back to button");
	g_registeredTypes.insert("wasd");
	g_menuChoices.clear();
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	Check(MenuOf(1) != nullptr && MenuOf(1)->type == "hud", "HUD_LAYOUT default -> hud");

	// A selection only queues; the handler performs no DB/menu work.
	FakeMenu* menu = MenuOf(1);
	Check(menu != nullptr, "the main menu is displayed");
	const std::size_t createdBefore = g_createdMenus.size();
	Check(menu != nullptr && ItemIndex(*menu, "info") >= 0, "the main menu has the info item");
	SelectMenu(1, "info");
	CheckEqInt(static_cast<int64_t>(g_createdMenus.size()), static_cast<int64_t>(createdBefore),
			   "the menu handler only queues the action");
	CheckEqInt(static_cast<int64_t>(k4::g_pendingActions.size()), 1, "one action is queued");
	// The slot is reused before OnPluginUpdate drains the queue: the action must be dropped.
	g_clients[1].steam = 76561198000000099ULL;
	Update();
	CheckEqInt(static_cast<int64_t>(g_createdMenus.size()), static_cast<int64_t>(createdBefore),
			   "a reused slot drops the queued selection");
	// Restore the identity and select again: now it is applied and the info menu opens.
	g_clients[1].steam = 76561198000000001ULL;
	SelectMenu(1, "info");
	Update();
	Settle();
	Check(MenuOf(1) != nullptr &&
			  MenuOf(1)->title == k4::StripColorTags(k4::Phrase("k4.menu.title.info")),
		  "the info menu replaced the main menu after the deferred action");

	// OnMapEnd: CancelClientMenu(Disconnect) + GetClientMenu recheck + no-op type switch, no
	// backend close on the dying client layer.
	g_cancelLeavesState = true; // the display survived the cancel: the type switch must save it
	ResetCallCounters();
	g_backendCloses = 0; // replacement closes during the test were legitimate (live map)
	FireMapEnd();
	CheckEqInt(g_backendCloses, 0, "no backend close in the changelevel window");
	bool infoDestroyed = false;
	for (const s2sdk::MenuId id : g_destroyedMenus) {
		if (id != 0) infoDestroyed = true;
	}
	Check(infoDestroyed, "the displayed menu was destroyed at map end");
	CheckEqInt(g_clientCalls, 0, "no client call from OnMapEnd");
	CheckEqInt(g_schemaCalls, 0, "no schema call from OnMapEnd");
	g_cancelLeavesState = false;

	// The same teardown at unload: the reserved dummy id 0 goes too.
	StopPlugin();
	bool reservedDestroyed = false;
	for (const s2sdk::MenuId id : g_destroyedMenus) {
		if (id == 0) reservedDestroyed = true;
	}
	Check(reservedDestroyed, "the reserved MenuId 0 is destroyed on unload");

	// A menu s2sdk refuses to release is parked and finally reported as A7_UNRESOLVED.
	PrepareTest();
	SeedClan(2, true);
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	StartPlugin();
	OpenMap();
	Settle();
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	Check(MenuOf(1) != nullptr, "menu displayed before the failing teardown");
	g_cancelLeavesState = true;
	g_destroyFails = true;
	g_logs.clear();
	FireMapEnd();
	Check(LoggedWith(plg::Severity::Warning, "A7_UNRESOLVED (parked)"),
		  "an unreleasable menu is parked with a warning");
	g_logs.clear();
	StopPlugin();
	Check(LoggedWith(plg::Severity::Error, "A7_UNRESOLVED: s2sdk refused to release menu id(s)"),
		  "unload reports the unresolved menu ids");
	g_destroyFails = false;
	g_cancelLeavesState = false;
}

void TestJoinFlow() {
	Section("open-clan join through the menu (SQLite)");
	PrepareTest();
	SeedClan(1, false);
	AddHuman(3, 76561198000000003ULL, 2, "Newbie");
	StartPlugin();
	OpenMap();
	Settle();
	k4::g_lastCommandAt[3] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 3, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	FakeMenu* main = MenuOf(3);
	Check(main != nullptr && ItemIndex(*main, "clans") >= 0, "the no-clan menu lists the clans");
	SelectMenu(3, "clans");
	Settle();
	FakeMenu* list = MenuOf(3);
	Check(list != nullptr && ItemIndex(*list, "clan:1") >= 0, "the clan list shows clan 1");
	SelectMenu(3, "clan:1");
	Settle();
	FakeMenu* info = MenuOf(3);
	Check(info != nullptr && ItemIndex(*info, "join:1") >= 0, "an open clan offers Join");
	SelectMenu(3, "join:1");
	Settle();
	CheckEqInt(QueryInt("SELECT COUNT(*) FROM clans_players WHERE auth = '76561198000000003'"), 1,
			   "Join inserted the membership");
	CheckEqInt(QueryInt("SELECT clan_id FROM clans_players WHERE auth = '76561198000000003'"), 1,
			   "the membership points at clan 1");
	FakeMenu* afterJoin = MenuOf(3);
	Check(afterJoin != nullptr && ItemIndex(*afterJoin, "info") >= 0,
		  "the member menu is displayed after the join");
	// The site sees the row the game wrote; no log row while logs.on is false.
	CheckEqInt(QueryInt("SELECT COUNT(*) FROM clans_logs"), 0, "logging is off by default");
	StopPlugin();
}

void TestInviteAndKickIdentity() {
	Section("invite/kick actions are bound to the SteamID64 + controller handle");
	PrepareTest();
	SeedClan(1, true);
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	AddHuman(2, 76561198000000003ULL, 3, "Guest");
	StartPlugin();
	OpenMap();
	Settle();

	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	SelectMenu(1, "management");
	Settle();
	SelectMenu(1, "invite");
	Settle();
	FakeMenu* inviteList = MenuOf(1);
	Check(inviteList != nullptr && ItemIndex(*inviteList, "invite:76561198000000003") >= 0,
		  "the invite list shows the online guest");
	SelectMenu(1, "invite:76561198000000003");
	Settle();
	FakeMenu* confirm = MenuOf(1);
	Check(confirm != nullptr && ItemIndex(*confirm, "invite_do:76561198000000003") >= 0,
		  "the invite confirmation is displayed");
	SelectMenu(1, "invite_do:76561198000000003");
	Settle();
	FakeMenu* prompt = MenuOf(2);
	Check(prompt != nullptr && ItemIndex(*prompt, "invacc:1") >= 0,
		  "the invitee receives the accept/decline prompt");

	// The guest's slot is reused before the accept is drained: the action must not join the new
	// player and must not consume the invite for anybody.
	g_clients[2].steam = 76561198000000099ULL;
	SelectMenu(2, "invacc:1");
	Update();
	Settle();
	CheckEqInt(QueryInt("SELECT COUNT(*) FROM clans_players WHERE auth = '76561198000000099'"), 0,
			   "a reused slot never applies another player's invite");
	CheckEqInt(QueryInt("SELECT COUNT(*) FROM clans_players WHERE auth = '76561198000000003'"), 0,
			   "the invite is still pending after the dropped action");
	// The same identity selects again: now the invite is accepted.
	g_clients[2].steam = 76561198000000003ULL;
	SelectMenu(2, "invacc:1");
	Update();
	Settle();
	CheckEqInt(QueryInt("SELECT COUNT(*) FROM clans_players WHERE auth = '76561198000000003'"), 1,
			   "the invited player joins on the second accept");

	// Kick: the member's controller handle changes while the write is in flight; the "kicked"
	// notice must not reach the new occupant of the slot (the SteamID lookup still finds it).
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	SelectMenu(1, "management");
	Settle();
	SelectMenu(1, "kick");
	Settle();
	FakeMenu* kickList = MenuOf(1);
	Check(kickList != nullptr && ItemIndex(*kickList, "kick:76561198000000003") >= 0,
		  "the kick list shows the member");
	SelectMenu(1, "kick:76561198000000003");
	Settle();
	SelectMenu(1, "kick_do:76561198000000003");
	CheckEqInt(QueryInt("SELECT COUNT(*) FROM clans_players WHERE auth = '76561198000000003'"), 1,
			   "the membership exists before the kick write");
	// The queued kick action runs on the owner's identity; the target identity is captured when
	// the write callback resolves the slot. Change the target's controller serial so it no longer
	// matches.
	g_chat.clear();
	g_clients[2].controllerHandle = static_cast<int32_t>(0x40000000u | 0x1000u | 2u);
	Update();
	Settle();
	CheckEqInt(QueryInt("SELECT COUNT(*) FROM clans_players WHERE auth = '76561198000000003'"), 0,
			   "the kick removed the membership");
	bool memberNotified = false;
	for (const auto& [slot, text] : g_chat) {
		if (slot == 2 && text.find(k4::Phrase("k4.menu.chat.kicked_target")) != std::string::npos) {
			memberNotified = true;
		}
	}
	Check(!memberNotified, "a reused controller identity gets no 'kicked' notice");
	StopPlugin();
}

void TestDatabaseFallbackAndPriority() {
	Section("config/phrase priority and the databases.cfg fail-soft fallback");
	PrepareTest();
	std::filesystem::remove(DatabasesPath());
	WriteTextFile(CoreConfigPath(), "\"Core\"\n{\n    \"ServerLang\" \"russian\"\n}\n");
	StartPlugin();
	Check(LoggedWith(plg::Severity::Error, "falling back to a local SQLite database"),
		  "a missing databases.cfg logs one fallback error");
	Check(k4::g_database != nullptr, "the plugin still has a database");
	Check(k4::g_language == "ru", "core.cfg ServerLang russian -> ru");
	Check(k4::Phrase("k4.command.help.header").find("Команды клана") != std::string::npos,
		  "the Russian phrases are loaded from the plugin translations dir");
	Check(!LoggedWith(plg::Severity::Error, "cannot read"), "no noisy per-file error");
	StopPlugin();

	// s2sdk ServerLanguage is the second choice when core.cfg has no ServerLang.
	PrepareTest();
	WriteTextFile(CoreConfigPath(), "\"Core\"\n{\n}\n");
	g_sdkLanguage = "German";
	StartPlugin();
	Check(k4::g_language == "de", "core.cfg without ServerLang falls to s2sdk ServerLanguage");
	Check(k4::Phrase("k4.command.help.header").find("Clan Commands") != std::string::npos,
		  "a missing language falls back to the English phrase text");
	StopPlugin();

	// Missing config.json: defaults, still Ok.
	PrepareTest();
	std::filesystem::remove(ConfigPath());
	StartPlugin();
	Check(LoggedWith(plg::Severity::Error, "cannot read"), "a missing config.json is logged");
	Check(k4::g_config.membershipRefreshSeconds == 30, "built-in defaults are used");
	StopPlugin();
}

void TestTeardownAndReload() {
	Section("teardown (A7) and reload");
	PrepareTest();
	SeedClan(2, true);
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	StartPlugin();
	OpenMap();
	Settle();
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	Check(MenuOf(1) != nullptr, "a menu is displayed before the unload");
	StopPlugin();
	CheckEqInt(g_mapStartUnregisters, 1, "OnMapStart unregistered");
	CheckEqInt(g_mapEndUnregisters, 1, "OnMapEnd unregistered");
	Check(g_eventHooks.empty(), "every event unhooked");
	Check(g_commands.empty(), "every command and listener removed");
	bool removedClans = false;
	for (const std::string& name : g_removedCommands) {
		if (name == "clans") removedClans = true;
	}
	Check(removedClans, "an owned command is removed with RemoveCommand");
	Check(g_createdMenus.size() >= 2, "the displayed menu and the dummy were created");
	Check(!k4::g_database, "the database was released");
	CheckEqInt(static_cast<int64_t>(k4::g_players.size()), 0, "the player cache was dropped");

	// A reload registers everything exactly once again.
	const int startsBefore = g_mapStartRegisters;
	const int hooksBefore = g_hookCalls;
	const std::size_t menusBefore = g_createdMenus.size();
	StartPlugin();
	CheckEqInt(g_mapStartRegisters, startsBefore + 1, "OnMapStart registered again");
	CheckEqInt(g_hookCalls, hooksBefore + 5, "the events are hooked again");
	CheckEqInt(static_cast<int64_t>(g_createdMenus.size()),
			   static_cast<int64_t>(menusBefore) + 1, "one dummy menu per load");
	StopPlugin();

	// Ownership: a pre-existing command name is torn down as a listener, never RemoveCommand.
	PrepareTest();
	g_commandCreated["clans"] = false;
	StartPlugin();
	CheckEqInt(g_commands.count("clans"), 1, "the pre-existing command keeps a listener");
	StopPlugin();
	bool listenerClans = false;
	bool commandClans = false;
	for (const std::string& name : g_removedListeners) {
		if (name == "clans") listenerClans = true;
	}
	for (const std::string& name : g_removedCommands) {
		if (name == "clans") commandClans = true;
	}
	Check(listenerClans && !commandClans, "a pre-existing command is detached as a listener");
}

void NullS2sdkBindings() {
	__s2sdk_IsClientConnected = nullptr;
	__s2sdk_IsClientInGame = nullptr;
	__s2sdk_IsClientSourceTV = nullptr;
	__s2sdk_IsFakeClient = nullptr;
	__s2sdk_GetClientName = nullptr;
	__s2sdk_GetClientTeam = nullptr;
	__s2sdk_GetClientSteamID64 = nullptr;
	__s2sdk_GetClientPawn = nullptr;
	__s2sdk_GetMaxClients = nullptr;
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	__s2sdk_EntPointerToEntHandle = nullptr;
	__s2sdk_IsValidEntHandle = nullptr;
	__s2sdk_IsValidEntPointer = nullptr;
	__s2sdk_GetCurrentMap = nullptr;
	__s2sdk_GetServerLanguage = nullptr;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapStart_Unregister = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnMapEnd_Unregister = nullptr;
	__s2sdk_HookEvent = nullptr;
	__s2sdk_UnhookEvent = nullptr;
	__s2sdk_GetEventPlayerSlot = nullptr;
	__s2sdk_GetEventInt = nullptr;
	__s2sdk_PrintToChat = nullptr;
	__s2sdk_AddConsoleCommand = nullptr;
	__s2sdk_RemoveCommand = nullptr;
	__s2sdk_AddCommandListener = nullptr;
	__s2sdk_RemoveCommandListener = nullptr;
	__s2sdk_CreateMenu = nullptr;
	__s2sdk_DestroyMenu = nullptr;
	__s2sdk_IsValidMenu = nullptr;
	__s2sdk_AddMenuItem = nullptr;
	__s2sdk_DisplayMenu = nullptr;
	__s2sdk_GetClientMenu = nullptr;
	__s2sdk_GetClientMenuOffset = nullptr;
	__s2sdk_CancelClientMenu = nullptr;
	__s2sdk_SetMenuType = nullptr;
	__s2sdk_GetMenuType = nullptr;
	__s2sdk_SetMenuPagination = nullptr;
	__s2sdk_SetMenuExitButton = nullptr;
	__s2sdk_SetMenuExitBackButton = nullptr;
	__s2sdk_SetMenuCloseOnSelect = nullptr;
	__s2sdk_IsMenuTypeRegistered = nullptr;
	__hudmenu_GetClientMenuType = nullptr;
	__s2sdk_GetSchemaOffset = nullptr;
	__s2sdk_GetSchemaChainOffset = nullptr;
	__s2sdk_GetEntData2 = nullptr;
	__s2sdk_GetGameRules = nullptr;
	__s2sdk_SetEntDataCString = nullptr;
	__s2sdk_CreateTimer = nullptr;
}

void TestZeroBindings() {
	Section("zero s2sdk bindings (A1 fail soft)");
	PrepareTest();
	NullS2sdkBindings();
	StartPlugin();
	CheckEqInt(g_mapStartRegisters, 0, "no map listener registered");
	Check(g_eventHooks.empty(), "no event hooked");
	Check(g_commands.empty(), "no command registered");
	Check(g_createdMenus.empty(), "no menu created");
	Check(k4::g_database != nullptr, "the database still comes up");
	CheckEqInt(g_timerCalls, 0, "still no s2sdk timer");
	Update();
	Check(!k4::g_mapReady, "the gate cannot open without GetCurrentMap");
	Check(!LoggedWith(plg::Severity::Error, "OnPluginStart failed"),
		  "missing bindings never fail the load");
	StopPlugin();
}

void TestIdentityHandleStrictness() {
	Section("strict controller handle: fail-closed bindings and same-SteamID reuse");
	// A complete s2sdk: the menu works and the deferred action is applied.
	PrepareTest();
	SeedClan(2, true);
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	StartPlugin();
	OpenMap();
	Settle();
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	Check(MenuOf(1) != nullptr, "the menu opens with a complete s2sdk");
	const std::size_t createdBefore = g_createdMenus.size();
	// Same SteamID64, new controller serial between the click and OnPluginUpdate: the queued
	// action must be dropped even though the slot still has "the same" player id.
	SelectMenu(1, "info");
	Check(g_clients[1].controllerHandle == DefaultControllerHandle(1), "the click carries the old handle");
	g_clients[1].controllerHandle = static_cast<int32_t>(0x40000000u | 0x2u);
	Update();
	Settle();
	CheckEqInt(static_cast<int64_t>(g_createdMenus.size()), static_cast<int64_t>(createdBefore),
			   "the same SteamID with a new controller handle drops the queued action");
	Check(MenuOf(1) != nullptr, "the previous menu is still the displayed one");
	// The same handle returns: the same action is accepted again.
	g_clients[1].controllerHandle = DefaultControllerHandle(1);
	SelectMenu(1, "info");
	Update();
	Settle();
	Check(MenuOf(1) != nullptr &&
			  MenuOf(1)->title == k4::StripColorTags(k4::Phrase("k4.menu.title.info")),
		  "the action runs once the original handle is back");
	StopPlugin();

	// Missing __s2sdk_PlayerSlotToEntHandle: HasController is false, no menu and no tag write.
	PrepareTest();
	SeedClan(2, true);
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	StartPlugin();
	OpenMap();
	Settle();
	const std::size_t menusBefore = g_createdMenus.size();
	g_tagWrites.clear();
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	Check(!k4::HasController(1), "missing handle accessor -> HasController false (no handle 0 fallback)");
	Check(!k4::SlotIdentity(1).Valid(), "no valid identity without the handle accessor");
	k4::ApplyScoreboardTag(1, "TST");
	Check(g_tagWrites.empty(), "no tag write without the handle accessor");
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	CheckEqInt(static_cast<int64_t>(g_createdMenus.size()), static_cast<int64_t>(menusBefore),
			   "no menu opens without the handle accessor");
	StopPlugin();

	// Missing __s2sdk_IsValidEntHandle: the same fail-closed behaviour.
	PrepareTest();
	SeedClan(2, true);
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	StartPlugin();
	OpenMap();
	Settle();
	const std::size_t menusBeforeValidator = g_createdMenus.size();
	g_tagWrites.clear();
	__s2sdk_IsValidEntHandle = nullptr;
	Check(!k4::HasController(1), "missing handle validator -> HasController false");
	k4::ApplyScoreboardTag(1, "TST");
	Check(g_tagWrites.empty(), "no tag write without the handle validator");
	k4::g_lastCommandAt[1] = std::chrono::steady_clock::time_point{};
	RunCommand("clan", 1, s2sdk::ConCommandContext::Chat, {"clan"});
	Settle();
	CheckEqInt(static_cast<int64_t>(g_createdMenus.size()),
			   static_cast<int64_t>(menusBeforeValidator),
			   "no menu opens without the handle validator");
	StopPlugin();

	// Missing controller pointer / pointer validator: also fail closed.
	PrepareTest();
	AddHuman(1, 76561198000000001ULL, 2, "Owner");
	StartPlugin();
	OpenMap();
	Settle();
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	Check(!k4::HasController(1), "missing pointer accessor -> HasController false");
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_IsValidEntPointer = nullptr;
	Check(!k4::HasController(1), "missing pointer validator -> HasController false");
	StopPlugin();
}

} // namespace

int main() {
	TestStartupAndMapGate();
	TestHandleAndSchemaGuards();
	TestEventsAndKillRewards();
	TestCommandsAndChat();
	TestNativeMenus();
	TestJoinFlow();
	TestInviteAndKickIdentity();
	TestDatabaseFallbackAndPriority();
	TestTeardownAndReload();
	TestZeroBindings();
	TestIdentityHandleStrictness();

	std::error_code cleanupError;
	if (!g_baseDir.empty()) std::filesystem::remove_all(g_baseDir, cleanupError);

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
