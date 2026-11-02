#include "k4_store.hpp"

#include <ctime>
#include <optional>
#include <utility>

namespace k4 {
namespace {

std::size_t Column(const plgdb::Result& result, const char* name, std::size_t fallback) {
	const std::optional<std::size_t> index = result.ColumnIndex(name);
	return index.has_value() ? *index : fallback;
}

int64_t Now() {
	return static_cast<int64_t>(std::time(nullptr));
}

struct LoadState {
	std::unique_ptr<Guild> guild;
	int remaining = 0;
	std::string error;
	std::function<void(std::unique_ptr<Guild>, const std::string&)> callback;

	void Record(const plgdb::Result& result) {
		if (error.empty() && !result.Ok()) {
			error = result.Error();
		}
	}

	void Finish() {
		if (remaining > 0) {
			--remaining;
		}
		if (remaining == 0 && callback) {
			callback(std::move(guild), error);
		}
	}
};

void QueryTop(plgdb::Database& database, std::string sql, plgdb::Params params,
			  std::function<void(std::vector<TopEntry>)> callback) {
	database.Query(std::move(sql), std::move(params),
				   [callback = std::move(callback)](const plgdb::Result& result) {
					   std::vector<TopEntry> entries;
					   if (!result.Ok()) {
						   callback(std::move(entries));
						   return;
					   }
					   const std::size_t idColumn = Column(result, "id", 0);
					   const std::size_t nameColumn = Column(result, "name", 1);
					   const std::size_t valueColumn = Column(result, "value", 2);
					   entries.reserve(result.RowCount());
					   for (std::size_t row = 0; row < result.RowCount(); ++row) {
						   TopEntry entry;
						   entry.clanId = static_cast<int32_t>(result.GetInt(row, idColumn));
						   entry.name = result.GetString(row, nameColumn);
						   entry.value = result.GetDouble(row, valueColumn);
						   entries.push_back(std::move(entry));
					   }
					   callback(std::move(entries));
				   });
}

} // namespace

void GuildStore::LoadGuild(int32_t guildId, GuildCallback callback) {
	LoadGuildChecked(
		guildId, [callback = std::move(callback)](std::unique_ptr<Guild> guild, const std::string&) {
			if (callback) {
				callback(std::move(guild));
			}
		});
}

void GuildStore::LoadGuildChecked(int32_t guildId, GuildLoadCallback callback) {
	const std::string sql =
		"SELECT id, name, tag, `desc`, owner, bank, slots, type, exp, role_count, created, lastupdate "
		"FROM clans_main WHERE id = ?";
	m_database.Query(sql, {static_cast<int64_t>(guildId)},
					 [this, callback = std::move(callback), guildId](const plgdb::Result& result) {
						 if (!result.Ok()) {
							 callback(nullptr, result.Error());
							 return;
						 }
						 if (result.RowCount() == 0) {
							 callback(nullptr, std::string());
							 return;
						 }
						 auto guild = std::make_unique<Guild>();
						 guild->id = static_cast<int32_t>(result.GetInt(0, Column(result, "id", 0)));
						 guild->name = result.GetString(0, Column(result, "name", 1));
						 guild->tag = result.GetString(0, Column(result, "tag", 2));
						 guild->desc = result.GetString(0, Column(result, "desc", 3));
						 guild->ownerAuth = result.GetString(0, Column(result, "owner", 4));
						 guild->bank = static_cast<int32_t>(result.GetInt(0, Column(result, "bank", 5)));
						 guild->slots = static_cast<int32_t>(result.GetInt(0, Column(result, "slots", 6)));
						 guild->type = static_cast<int32_t>(result.GetInt(0, Column(result, "type", 7), 1));
						 if (guild->type < 1 || guild->type > 3) {
							 guild->type = 1;
						 }
						 guild->exp = static_cast<int32_t>(result.GetInt(0, Column(result, "exp", 8)));
						 guild->roleCount =
							 static_cast<int32_t>(result.GetInt(0, Column(result, "role_count", 9)));
						 guild->created = result.GetInt(0, Column(result, "created", 10));
						 guild->lastupdate = result.GetInt(0, Column(result, "lastupdate", 11));

						 auto state = std::make_shared<LoadState>();
						 state->guild = std::move(guild);
						 state->remaining = 4;
						 state->callback = callback;

						 m_database.Query(
							 "SELECT id, auth, name, clan_id, kills, deaths, joined, lastactivity, role_id "
							 "FROM clans_players WHERE clan_id = ? ORDER BY id",
							 {static_cast<int64_t>(guildId)},
							 [state](const plgdb::Result& rows) {
								 state->Record(rows);
								 if (rows.Ok()) {
									 Guild& target = *state->guild;
									 const std::size_t idColumn = Column(rows, "id", 0);
									 const std::size_t authColumn = Column(rows, "auth", 1);
									 const std::size_t nameColumn = Column(rows, "name", 2);
									 const std::size_t clanColumn = Column(rows, "clan_id", 3);
									 const std::size_t killsColumn = Column(rows, "kills", 4);
									 const std::size_t deathsColumn = Column(rows, "deaths", 5);
									 const std::size_t joinedColumn = Column(rows, "joined", 6);
									 const std::size_t activityColumn = Column(rows, "lastactivity", 7);
									 const std::size_t roleColumn = Column(rows, "role_id", 8);
									 target.members.reserve(rows.RowCount());
									 for (std::size_t row = 0; row < rows.RowCount(); ++row) {
										 Member member;
										 member.id = rows.GetInt(row, idColumn);
										 member.auth = rows.GetString(row, authColumn);
										 member.name = rows.GetString(row, nameColumn);
										 member.clanId = static_cast<int32_t>(rows.GetInt(row, clanColumn));
										 member.kills = static_cast<int32_t>(rows.GetInt(row, killsColumn));
										 member.deaths = static_cast<int32_t>(rows.GetInt(row, deathsColumn));
										 member.joined = rows.GetInt(row, joinedColumn);
										 member.lastactivity = rows.GetInt(row, activityColumn);
										 member.roleId = rows.IsNull(row, roleColumn)
															? 0
															: static_cast<int32_t>(rows.GetInt(row, roleColumn));
										 target.members.push_back(std::move(member));
									 }
								 }
								 state->Finish();
							 });

						 m_database.Query(
							 "SELECT id, clan_id, name, type, kick, roles, invite, settings "
							 "FROM clans_roles WHERE clan_id = ? ORDER BY type ASC, id ASC",
							 {static_cast<int64_t>(guildId)},
							 [state](const plgdb::Result& rows) {
								 state->Record(rows);
								 if (rows.Ok()) {
									 Guild& target = *state->guild;
									 const std::size_t idColumn = Column(rows, "id", 0);
									 const std::size_t clanColumn = Column(rows, "clan_id", 1);
									 const std::size_t nameColumn = Column(rows, "name", 2);
									 const std::size_t typeColumn = Column(rows, "type", 3);
									 const std::size_t kickColumn = Column(rows, "kick", 4);
									 const std::size_t rolesColumn = Column(rows, "roles", 5);
									 const std::size_t inviteColumn = Column(rows, "invite", 6);
									 const std::size_t settingsColumn = Column(rows, "settings", 7);
									 target.roles.reserve(rows.RowCount());
									 for (std::size_t row = 0; row < rows.RowCount(); ++row) {
										 Role role;
										 role.id = static_cast<int32_t>(rows.GetInt(row, idColumn));
										 role.clanId = static_cast<int32_t>(rows.GetInt(row, clanColumn));
										 role.name = rows.GetString(row, nameColumn);
										 role.type = static_cast<int32_t>(rows.GetInt(row, typeColumn, 3));
										 role.kick = rows.GetInt(row, kickColumn) != 0;
										 role.roles = rows.GetInt(row, rolesColumn) != 0;
										 role.invite = rows.GetInt(row, inviteColumn) != 0;
										 role.settings = rows.GetInt(row, settingsColumn) != 0;
										 target.roles.push_back(std::move(role));
									 }
								 }
								 state->Finish();
							 });

						 m_database.Query(
							 "SELECT slots_up, roles_up FROM clans_upgrade WHERE clan_id = ?",
							 {static_cast<int64_t>(guildId)},
							 [state](const plgdb::Result& rows) {
								 state->Record(rows);
								 if (rows.Ok() && rows.RowCount() > 0) {
									 state->guild->slotsUp = static_cast<int32_t>(
										 rows.GetInt(0, Column(rows, "slots_up", 0)));
									 state->guild->rolesUp = static_cast<int32_t>(
										 rows.GetInt(0, Column(rows, "roles_up", 1)));
								 }
								 state->Finish();
							 });

						 state->Finish(); // the main row itself
					 });
}

void GuildStore::LoadClanId(const std::string& auth, std::function<void(int32_t)> callback) {
	LoadClanIdChecked(auth, [callback = std::move(callback)](int32_t clanId, const std::string&) {
		if (callback) {
			callback(clanId);
		}
	});
}

void GuildStore::LoadClanIdChecked(
	const std::string& auth,
	std::function<void(int32_t, const std::string&)> callback) {
	m_database.Query("SELECT clan_id FROM clans_players WHERE auth = ? ORDER BY id LIMIT 1", {auth},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (!result.Ok()) {
							 callback(0, result.Error());
							 return;
						 }
						 if (result.RowCount() == 0) {
							 callback(0, std::string());
							 return;
						 }
						 callback(static_cast<int32_t>(result.GetInt(0, 0)), std::string());
					 });
}

void GuildStore::LoadGuildForPlayer(const std::string& auth, GuildCallback callback) {
	LoadGuildForPlayerChecked(
		auth, [callback = std::move(callback)](std::unique_ptr<Guild> guild, const std::string&) {
			if (callback) {
				callback(std::move(guild));
			}
		});
}

void GuildStore::LoadGuildForPlayerChecked(const std::string& auth, GuildLoadCallback callback) {
	LoadClanIdChecked(auth, [this, callback = std::move(callback)](int32_t clanId,
																  const std::string& error) {
		if (!error.empty()) {
			callback(nullptr, error);
			return;
		}
		if (clanId <= 0) {
			callback(nullptr, std::string());
			return;
		}
		LoadGuildChecked(clanId, std::move(callback));
	});
}

void GuildStore::AddBank(int32_t guildId, int32_t delta, std::function<void(bool)> callback) {
	if (delta == 0) {
		if (callback) {
			callback(true);
		}
		return;
	}
	const int64_t amount = delta > 0 ? static_cast<int64_t>(delta) : -static_cast<int64_t>(delta);
	const std::string_view sql = delta > 0 ? BankAddSql() : BankSubtractSql();
	m_database.Query(std::string(sql), {amount, amount, static_cast<int64_t>(guildId)},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (callback) {
							 callback(result.Ok());
						 }
					 });
}

void GuildStore::AddKillStats(const std::string& attackerAuth, const std::string& victimAuth,
							  std::function<void(bool)> callback) {
	const int64_t now = Now();
	std::vector<plgdb::Statement> statements;
	if (!attackerAuth.empty()) {
		plgdb::Statement statement;
		statement.sql = std::string(KillsIncrementSql());
		statement.params.emplace_back(now);
		statement.params.emplace_back(attackerAuth);
		statements.push_back(std::move(statement));
	}
	if (!victimAuth.empty()) {
		plgdb::Statement statement;
		statement.sql = std::string(DeathsIncrementSql());
		statement.params.emplace_back(now);
		statement.params.emplace_back(victimAuth);
		statements.push_back(std::move(statement));
	}
	if (statements.empty()) {
		if (callback) {
			callback(true);
		}
		return;
	}
	if (statements.size() == 1) {
		m_database.Query(statements.front().sql, std::move(statements.front().params),
						 [callback = std::move(callback)](const plgdb::Result& result) {
							 if (callback) {
								 callback(result.Ok());
							 }
						 });
		return;
	}
	m_database.Transaction(std::move(statements),
						   [callback = std::move(callback)](const plgdb::Result& result) {
							   if (callback) {
								   callback(result.Ok());
							   }
						   });
}

void GuildStore::TouchMember(const std::string& auth, const std::string& name,
							 std::function<void()> callback) {
	const int64_t now = Now();
	if (name.empty()) {
		m_database.Query(std::string(TouchActivitySql()), {now, auth},
						 [callback = std::move(callback)](const plgdb::Result&) {
							 if (callback) {
								 callback();
							 }
						 });
		return;
	}
	m_database.Query(std::string(TouchMemberSql()), {name, now, auth},
					 [callback = std::move(callback)](const plgdb::Result&) {
						 if (callback) {
							 callback();
						 }
					 });
}

void GuildStore::LoadClanList(int32_t limit,
							  std::function<void(std::vector<ClanSummary>)> callback) {
	m_database.Query(
		"SELECT c.id AS id, c.name AS name, c.tag AS tag, c.type AS type, c.bank AS bank, "
		"c.exp AS exp, c.slots AS slots, COALESCE(u.slots_up, 0) AS slots_up, "
		"(SELECT COUNT(*) FROM clans_players p WHERE p.clan_id = c.id) AS members "
		"FROM clans_main c LEFT JOIN clans_upgrade u ON u.clan_id = c.id "
		"ORDER BY c.exp DESC, c.id ASC LIMIT ?",
		{static_cast<int64_t>(limit)},
		[callback = std::move(callback)](const plgdb::Result& result) {
			std::vector<ClanSummary> clans;
			if (!result.Ok()) {
				callback(std::move(clans));
				return;
			}
			const std::size_t idColumn = Column(result, "id", 0);
			const std::size_t nameColumn = Column(result, "name", 1);
			const std::size_t tagColumn = Column(result, "tag", 2);
			const std::size_t typeColumn = Column(result, "type", 3);
			const std::size_t bankColumn = Column(result, "bank", 4);
			const std::size_t expColumn = Column(result, "exp", 5);
			const std::size_t slotsColumn = Column(result, "slots", 6);
			const std::size_t slotsUpColumn = Column(result, "slots_up", 7);
			const std::size_t membersColumn = Column(result, "members", 8);
			clans.reserve(result.RowCount());
			for (std::size_t row = 0; row < result.RowCount(); ++row) {
				ClanSummary clan;
				clan.id = static_cast<int32_t>(result.GetInt(row, idColumn));
				clan.name = result.GetString(row, nameColumn);
				clan.tag = result.GetString(row, tagColumn);
				clan.type = static_cast<int32_t>(result.GetInt(row, typeColumn, 1));
				clan.bank = static_cast<int32_t>(result.GetInt(row, bankColumn));
				clan.exp = static_cast<int32_t>(result.GetInt(row, expColumn));
				clan.slots = static_cast<int32_t>(result.GetInt(row, slotsColumn));
				clan.slotsUp = static_cast<int32_t>(result.GetInt(row, slotsUpColumn));
				clan.members = static_cast<int32_t>(result.GetInt(row, membersColumn));
				clans.push_back(std::move(clan));
			}
			callback(std::move(clans));
		});
}

void GuildStore::LoadRequests(int32_t guildId,
							  std::function<void(std::vector<ClanRequest>)> callback) {
	m_database.Query(
		"SELECT id, clan_id, steamid, name, date FROM clans_request WHERE clan_id = ? "
		"ORDER BY date ASC, id ASC",
		{static_cast<int64_t>(guildId)},
		[callback = std::move(callback)](const plgdb::Result& result) {
			std::vector<ClanRequest> requests;
			if (!result.Ok()) {
				callback(std::move(requests));
				return;
			}
			const std::size_t idColumn = Column(result, "id", 0);
			const std::size_t clanColumn = Column(result, "clan_id", 1);
			const std::size_t authColumn = Column(result, "steamid", 2);
			const std::size_t nameColumn = Column(result, "name", 3);
			const std::size_t dateColumn = Column(result, "date", 4);
			requests.reserve(result.RowCount());
			for (std::size_t row = 0; row < result.RowCount(); ++row) {
				ClanRequest request;
				request.id = result.GetInt(row, idColumn);
				request.clanId = static_cast<int32_t>(result.GetInt(row, clanColumn));
				request.auth = result.GetString(row, authColumn);
				request.name = result.GetString(row, nameColumn);
				request.date = result.GetInt(row, dateColumn);
				requests.push_back(std::move(request));
			}
			callback(std::move(requests));
		});
}

void GuildStore::HasPendingRequest(int32_t guildId, const std::string& auth,
								   std::function<void(bool)> callback) {
	m_database.Query("SELECT id FROM clans_request WHERE clan_id = ? AND steamid = ? LIMIT 1",
					 {static_cast<int64_t>(guildId), auth},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (callback) {
							 callback(result.Ok() && result.RowCount() > 0);
						 }
					 });
}

void GuildStore::JoinClan(int32_t guildId, const std::string& auth, const std::string& name,
						  int slotsStep, bool requireOpenType,
						  std::function<void(bool)> callback) {
	const bool mysql = m_database.GetDriver() == plgdb::Driver::MySQL;
	const int64_t now = Now();
	plgdb::Params params;
	params.emplace_back(auth);
	params.emplace_back(name);
	params.emplace_back(static_cast<int64_t>(guildId));
	params.emplace_back(now);
	params.emplace_back(now);
	params.emplace_back(auth); // not a member of any clan
	if (requireOpenType) {
		params.emplace_back(static_cast<int64_t>(guildId)); // type = 1
	}
	params.emplace_back(static_cast<int64_t>(guildId)); // current member count
	params.emplace_back(static_cast<int64_t>(slotsStep));
	params.emplace_back(static_cast<int64_t>(guildId)); // effective slots subquery
	m_database.Query(JoinClanSql(requireOpenType, mysql), std::move(params),
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (callback) {
							 callback(result.Ok() && result.AffectedRows() > 0);
						 }
					 });
}

void GuildStore::ApplyToClan(int32_t guildId, const std::string& auth, const std::string& name,
							 std::function<void(bool)> callback) {
	const bool mysql = m_database.GetDriver() == plgdb::Driver::MySQL;
	const int64_t now = Now();
	m_database.Query(ApplyRequestSql(mysql),
					 {static_cast<int64_t>(guildId), auth, name, now,
					  static_cast<int64_t>(guildId), auth, static_cast<int64_t>(guildId), auth},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (callback) {
							 callback(result.Ok() && result.AffectedRows() > 0);
						 }
					 });
}

void GuildStore::CancelRequest(int32_t guildId, const std::string& auth,
							   std::function<void(bool)> callback) {
	m_database.Query(std::string(DeleteRequestByPlayerSql()),
					 {static_cast<int64_t>(guildId), auth},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (callback) {
							 callback(result.Ok() && result.AffectedRows() > 0);
						 }
					 });
}

void GuildStore::LeaveClan(int32_t guildId, const std::string& auth,
						   std::function<void(bool)> callback) {
	m_database.Query(std::string(LeaveClanSql()),
					 {static_cast<int64_t>(guildId), auth, static_cast<int64_t>(guildId)},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (callback) {
							 callback(result.Ok() && result.AffectedRows() > 0);
						 }
					 });
}

void GuildStore::KickMember(int32_t guildId, const std::string& actorAuth,
							const std::string& targetAuth,
							std::function<void(bool)> callback) {
	m_database.Query(std::string(KickMemberSql()),
					 {static_cast<int64_t>(guildId), targetAuth, static_cast<int64_t>(guildId),
					  actorAuth},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (callback) {
							 callback(result.Ok() && result.AffectedRows() > 0);
						 }
					 });
}

void GuildStore::AcceptRequest(int32_t guildId, int64_t requestId, const std::string& auth,
							   int slotsStep, std::function<void(bool)> callback) {
	// One transaction: an accept that cannot remove its request row must not leave the player
	// joined (the site's two statements are not atomic), and the guarded delete doubles as the
	// success report - plgdb returns only the last statement's result, and a refused insert
	// (stale request, player already in a clan, full clan) leaves nothing to delete.
	const int64_t now = Now();
	std::vector<plgdb::Statement> statements;
	statements.resize(2);
	statements[0].sql = std::string(AcceptRequestSql());
	statements[0].params = {now, now, requestId, static_cast<int64_t>(guildId),
							static_cast<int64_t>(slotsStep)};
	statements[1].sql = std::string(DeleteAcceptedRequestSql());
	statements[1].params = {requestId, static_cast<int64_t>(guildId), auth, auth,
							static_cast<int64_t>(guildId)};
	m_database.Transaction(std::move(statements),
						   [callback = std::move(callback)](const plgdb::Result& result) {
							   if (callback) {
								   callback(result.Ok() && result.AffectedRows() > 0);
							   }
						   });
}

void GuildStore::DeclineRequest(int32_t guildId, const std::string& auth,
								std::function<void(bool)> callback) {
	m_database.Query(std::string(DeleteRequestByPlayerSql()),
					 {static_cast<int64_t>(guildId), auth},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (callback) {
							 callback(result.Ok() && result.AffectedRows() > 0);
						 }
					 });
}

void GuildStore::DisbandClan(int32_t guildId, const std::string& ownerAuth, bool deleteLogs,
							 std::function<void(bool)> callback) {
	// One transaction in the site's table order (Clans::deleteClanCascade). On MySQL the first
	// statement locks the clan row; the cascade statements and the main delete repeat the owner
	// guard, and the last statement counts what is left. This replaces the former multi-table
	// DELETE, whose LEFT JOINs over the 1:N tables built a cartesian product and whose FK
	// handling/table order MySQL does not guarantee.
	const bool mysql = m_database.GetDriver() == plgdb::Driver::MySQL;
	const std::vector<std::string_view> sqls = DisbandCascadeSql(mysql, deleteLogs);
	const std::string lockSql(DisbandLockSql());
	const std::string mainSql(DisbandMainSql());
	const std::string remainingSql(DisbandRemainingSql());
	std::vector<plgdb::Statement> statements;
	statements.reserve(sqls.size());
	for (const std::string_view sql : sqls) {
		plgdb::Statement statement;
		statement.sql = std::string(sql);
		if (statement.sql == lockSql || statement.sql == remainingSql) {
			statement.params.emplace_back(static_cast<int64_t>(guildId));
		} else if (statement.sql == mainSql) {
			statement.params.emplace_back(static_cast<int64_t>(guildId));
			statement.params.emplace_back(ownerAuth);
		} else {
			// Every cascade statement: clan id + the owner-guard subquery (id, owner).
			statement.params.emplace_back(static_cast<int64_t>(guildId));
			statement.params.emplace_back(static_cast<int64_t>(guildId));
			statement.params.emplace_back(ownerAuth);
		}
		statements.push_back(std::move(statement));
	}
	m_database.Transaction(std::move(statements),
						   [callback = std::move(callback)](const plgdb::Result& result) {
							   const bool gone =
								   result.Ok() && result.RowCount() > 0 && result.GetInt(0, 0) == 0;
							   if (callback) {
								   callback(gone);
							   }
						   });
}

void GuildStore::WriteLog(int32_t clanId, const std::string& action, const std::string& valueJson,
						  std::function<void(bool)> callback) {
	m_database.Query(std::string(InsertLogSql()),
					 {static_cast<int64_t>(clanId), action, valueJson, Now()},
					 [callback = std::move(callback)](const plgdb::Result& result) {
						 if (callback) {
							 callback(result.Ok());
						 }
					 });
}

void GuildStore::TopBank(int32_t limit, std::function<void(std::vector<TopEntry>)> callback) {
	QueryTop(m_database,
			 "SELECT id, name, bank AS value FROM clans_main ORDER BY bank DESC, id ASC LIMIT ?",
			 {static_cast<int64_t>(limit)}, std::move(callback));
}

void GuildStore::TopExp(int32_t limit, std::function<void(std::vector<TopEntry>)> callback) {
	QueryTop(m_database,
			 "SELECT id, name, exp AS value FROM clans_main ORDER BY exp DESC, id ASC LIMIT ?",
			 {static_cast<int64_t>(limit)}, std::move(callback));
}

void GuildStore::TopKills(int32_t limit, bool deaths,
						  std::function<void(std::vector<TopEntry>)> callback) {
	const std::string column = deaths ? "deaths" : "kills";
	QueryTop(m_database,
			 "SELECT c.id AS id, c.name AS name, COALESCE(SUM(p." + column + "), 0) AS value "
			 "FROM clans_main c LEFT JOIN clans_players p ON p.clan_id = c.id "
			 "GROUP BY c.id, c.name ORDER BY value DESC, c.id ASC LIMIT ?",
			 {static_cast<int64_t>(limit)}, std::move(callback));
}

} // namespace k4
