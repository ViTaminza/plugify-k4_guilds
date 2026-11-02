// k4_guilds — live clans_* access on plgdb.
//
// The website still owns clan settings, roles, services, levels and duration; the plugin reads
// guilds/requests for the menu and writes only the actions the old Metamod plugin performed
// in-game (join/apply/cancel, leave, kick, request accept/decline, disband), mirroring the
// site's SQL from forward/data.php and Clans::deleteClanCascade, plus the kill-reward columns.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <plgdb/db.hpp>

#include "k4_core.hpp"

namespace k4 {

struct TopEntry {
	int32_t clanId = 0;
	std::string name;
	double value = 0.0;
};

struct ClanSummary {
	int32_t id = 0;
	std::string name;
	std::string tag;
	int32_t type = 1;
	int32_t bank = 0;
	int32_t exp = 0;
	int32_t slots = 0;
	int32_t slotsUp = 0;
	int32_t members = 0;
};

struct ClanRequest {
	int64_t id = 0;
	int32_t clanId = 0;
	std::string auth;
	std::string name;
	int64_t date = 0;
};

class GuildStore {
public:
	explicit GuildStore(plgdb::Database& database) : m_database(database) {}

	using GuildCallback = std::function<void(std::unique_ptr<Guild>)>;
	// Same as GuildCallback, but tells a failed query (non-empty error) from a valid "no clan"
	// answer (null guild, empty error): a transient database error must never look like a site
	// kick to the caller's membership cache.
	using GuildLoadCallback =
		std::function<void(std::unique_ptr<Guild> guild, const std::string& error)>;

	void LoadGuild(int32_t guildId, GuildCallback callback);
	void LoadGuildChecked(int32_t guildId, GuildLoadCallback callback);
	void LoadGuildForPlayer(const std::string& auth, GuildCallback callback);
	void LoadGuildForPlayerChecked(const std::string& auth, GuildLoadCallback callback);
	void LoadClanId(const std::string& auth, std::function<void(int32_t)> callback);
	void LoadClanIdChecked(const std::string& auth,
						   std::function<void(int32_t clanId, const std::string& error)> callback);
	void LoadClanList(int32_t limit, std::function<void(std::vector<ClanSummary>)> callback);
	void LoadRequests(int32_t guildId, std::function<void(std::vector<ClanRequest>)> callback);
	void HasPendingRequest(int32_t guildId, const std::string& auth,
						   std::function<void(bool)> callback);

	// --- kill-reward runtime columns --------------------------------------------------------
	void AddBank(int32_t guildId, int32_t delta, std::function<void(bool)> callback = {});
	void AddKillStats(const std::string& attackerAuth, const std::string& victimAuth,
					  std::function<void(bool)> callback = {});
	void TouchMember(const std::string& auth, const std::string& name,
					 std::function<void()> callback = {});

	// --- in-game clan actions (site SQL + race-safe guards) ---------------------------------
	// requireOpenType=true mirrors ClanJoin (open clans); false is used for accepted invites.
	void JoinClan(int32_t guildId, const std::string& auth, const std::string& name, int slotsStep,
				  bool requireOpenType, std::function<void(bool)> callback = {});
	void ApplyToClan(int32_t guildId, const std::string& auth, const std::string& name,
					 std::function<void(bool)> callback = {});
	void CancelRequest(int32_t guildId, const std::string& auth,
					   std::function<void(bool)> callback = {});
	void LeaveClan(int32_t guildId, const std::string& auth,
				   std::function<void(bool)> callback = {});
	void KickMember(int32_t guildId, const std::string& actorAuth, const std::string& targetAuth,
					std::function<void(bool)> callback = {});
	void AcceptRequest(int32_t guildId, int64_t requestId, const std::string& auth, int slotsStep,
					   std::function<void(bool)> callback = {});
	void DeclineRequest(int32_t guildId, const std::string& auth,
						std::function<void(bool)> callback = {});
	// deleteLogs mirrors the site's logs.delete_logs switch (live value: off).
	void DisbandClan(int32_t guildId, const std::string& ownerAuth, bool deleteLogs,
					 std::function<void(bool)> callback = {});

	// clans_logs row (Clans::addLog): action + JSON value + time() + status 1.
	void WriteLog(int32_t clanId, const std::string& action, const std::string& valueJson,
				  std::function<void(bool)> callback = {});

	// --- read-only tops ---------------------------------------------------------------------
	void TopBank(int32_t limit, std::function<void(std::vector<TopEntry>)> callback);
	void TopExp(int32_t limit, std::function<void(std::vector<TopEntry>)> callback);
	void TopKills(int32_t limit, bool deaths, std::function<void(std::vector<TopEntry>)> callback);

private:
	plgdb::Database& m_database;
};

} // namespace k4
