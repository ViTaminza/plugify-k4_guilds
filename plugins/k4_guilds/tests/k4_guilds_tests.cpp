// Engine-free tests of plugins/k4_guilds.
//
// Covers:
//   * config.json parsing (runtime-only keys, levels copy),
//   * the plugin-local KV1 reader and the pisexcompat file/lookup semantics: databases.cfg and
//     phrases.txt resolution order, the core.cfg -> s2sdk -> en language chain, phrase fallbacks,
//     the cs2-menus colour table and the database section parser,
//   * the s2sdk safety helpers: the entity-handle rule (C1) and pisexcompat's
//     ResolveS2sdkMenuTypeName table (C10),
//   * kill/death accounting: warmup, bots/invalid slots, suicides, same-team/same-clan rules,
//     bank clamp,
//   * the clamped SQL builders used against the site-owned clans_* tables,
//   * level derivation matching Clans::getClanLevelProgress,
//   * command parsing/aliases (read-only command set),
//   * a GuildStore test against a schema-only copy of the live clans tables: SQLite always and
//     MariaDB when ./test.sh exports MYSQL_* (the production driver).
//
// Run by ./test.sh; the executable lives in build/tests.

#include "k4_core.hpp"
#include "k4_files.hpp"
#include "k4_store.hpp"
#include "kv1.hpp"

#include <plgdb/db.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#ifndef K4_GUILDS_TEST_DATA_DIR
#define K4_GUILDS_TEST_DATA_DIR "."
#endif
#ifndef K4_GUILDS_TEST_PHRASES
#define K4_GUILDS_TEST_PHRASES "."
#endif
#ifndef K4_GUILDS_TEST_CONFIG
#define K4_GUILDS_TEST_CONFIG "."
#endif

namespace {

int g_checks = 0;
int g_failures = 0;

void Fail(const char* file, int line, const std::string& text) {
	++g_failures;
	std::printf("FAIL %s:%d: %s\n", file, line, text.c_str());
}

#define CHECK(condition) \
	do { \
		++g_checks; \
		if (!(condition)) { \
			Fail(__FILE__, __LINE__, #condition); \
		} \
	} while (0)

#define CHECK_EQ(actual, expected) \
	do { \
		++g_checks; \
		const auto& actualValue = (actual); \
		const auto& expectedValue = (expected); \
		if (!(actualValue == expectedValue)) { \
			std::ostringstream message; \
			message << #actual << " == " << #expected; \
			Fail(__FILE__, __LINE__, message.str()); \
		} \
	} while (0)

std::string ReadFile(const std::string& path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) {
		return {};
	}
	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

template <typename Done>
bool Pump(plgdb::Database& database, Done done, int timeoutMs = 5000) {
	const auto start = std::chrono::steady_clock::now();
	while (!done()) {
		database.Poll();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start)
				.count() > timeoutMs) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

// Waits until the worker has executed every queued statement (no callback needed).
bool PumpPending(plgdb::Database& database) {
	const bool done = Pump(database, [&database] { return database.Pending() == 0; });
	database.Poll();
	return done;
}

// ---------------------------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------------------------

void TestConfig() {
	k4::Config defaults;
	CHECK_EQ(defaults.killReward, 3);
	CHECK_EQ(defaults.deathPenalty, 2);
	CHECK(defaults.ignoreWarmup);
	CHECK(defaults.ignoreSameTeam);
	CHECK(!defaults.ignoreSameClan);
	CHECK(!defaults.countBots);
	CHECK_EQ(defaults.membershipRefreshSeconds, 30);
	CHECK_EQ(defaults.slotsStep, 10);
	CHECK_EQ(defaults.rolesStep, 3);
	CHECK(defaults.guildChat);
	CHECK(defaults.levels.empty());
	CHECK(!defaults.logs.on);
	CHECK(!defaults.logs.deleteLogs);
	CHECK(defaults.logs.join && defaults.logs.kick && defaults.logs.requestAccept);

	const std::string shipped = ReadFile(std::string(K4_GUILDS_TEST_DATA_DIR) + "/config.json");
	CHECK(!shipped.empty());
	k4::Config config;
	std::string error;
	CHECK(k4::ParseConfig(shipped, config, error));
	CHECK(error.empty());
	CHECK_EQ(config.killReward, 3);
	CHECK_EQ(config.deathPenalty, 2);
	CHECK_EQ(config.chatAliases.size(), std::size_t {4});
	CHECK_EQ(config.consoleAliases.size(), std::size_t {2});
	CHECK_EQ(config.slotsStep, 10);
	CHECK_EQ(config.rolesStep, 3);
	CHECK(!config.countBots); // bots are ignored by default (DESIGN §4)
	CHECK(!config.logs.on); // live site: logs.php 'on' => '0'
	CHECK(!config.logs.deleteLogs);
	CHECK(config.logs.join);
	CHECK_EQ(config.levels.size(), std::size_t {6});
	if (config.levels.size() == 6) {
		CHECK_EQ(config.levels[0].id, 1);
		CHECK_EQ(config.levels[0].exp, int64_t {0});
		CHECK(config.levels[0].name.empty()); // site name null
		CHECK_EQ(config.levels[5].id, 6);
		CHECK_EQ(config.levels[5].name, std::string("Legend"));
		CHECK_EQ(config.levels[5].exp, int64_t {5000});
	}

	const std::string text = R"json({
		// comment
		"kill_rewards": {"kill_reward": 7, "death_penalty": 4, "ignore_warmup": false,
						 "ignore_same_team": false, "ignore_same_clan": true, "count_bots": true},
		"membership": {"refresh_seconds": 12},
		"services": {"slots_step": 20, "roles_step": 5},
		"top": {"enable_bank": true},
		"logs": {"on": true, "delete_logs": true, "kick": false},
		"levels": [{"id": 1, "name": "Novice", "exp": 0}, {"id": 2, "name": "Veteran", "exp": 1000}]
	})json";
	k4::Config parsed;
	CHECK(k4::ParseConfig(text, parsed, error));
	CHECK_EQ(parsed.killReward, 7);
	CHECK_EQ(parsed.deathPenalty, 4);
	CHECK(!parsed.ignoreWarmup);
	CHECK(!parsed.ignoreSameTeam);
	CHECK(parsed.ignoreSameClan);
	CHECK(parsed.countBots);
	CHECK_EQ(parsed.membershipRefreshSeconds, 12);
	CHECK_EQ(parsed.slotsStep, 20);
	CHECK_EQ(parsed.rolesStep, 5);
	CHECK(parsed.topBank);
	CHECK_EQ(parsed.levels.size(), std::size_t {2});
	CHECK_EQ(parsed.levels[1].name, std::string("Veteran"));
	CHECK(parsed.logs.on);
	CHECK(parsed.logs.deleteLogs);
	CHECK(!parsed.logs.kick);
	CHECK(k4::ShouldLogAction(parsed, "join"));
	CHECK(!k4::ShouldLogAction(parsed, "kick"));
	CHECK(!k4::ShouldLogAction(defaults, "join")); // master switch off

	// The live site's levels.json stores the thresholds as strings; a file copied from it must
	// parse to the same numbers instead of silently collapsing every level to exp 0.
	const std::string siteShape = R"json({
		"levels": [{"id": 1, "name": null, "exp": "0"},
				   {"id": 2, "name": null, "exp": "1000"},
				   {"id": 6, "name": "Legend", "exp": "5000"}]
	})json";
	k4::Config fromSite;
	CHECK(k4::ParseConfig(siteShape, fromSite, error));
	CHECK(error.empty());
	CHECK_EQ(fromSite.levels.size(), std::size_t {3});
	if (fromSite.levels.size() == 3) {
		CHECK_EQ(fromSite.levels[0].exp, int64_t {0});
		CHECK_EQ(fromSite.levels[1].exp, int64_t {1000});
		CHECK_EQ(fromSite.levels[2].exp, int64_t {5000});
		CHECK_EQ(fromSite.levels[2].name, std::string("Legend"));
		CHECK(fromSite.levels[1].name.empty());
	}

	k4::Config bad;
	CHECK(!k4::ParseConfig("{ not json", bad, error));
	CHECK(!error.empty());
}

// ---------------------------------------------------------------------------------------------
// levels
// ---------------------------------------------------------------------------------------------

void TestLevels() {
	const std::vector<k4::ClanLevel> levels = {
		{1, "Novice", 0},
		{2, "Regular", 1000},
		{3, "Veteran", 5000},
	};

	k4::LevelInfo info = k4::DeriveLevel(0, levels);
	CHECK_EQ(info.name, std::string("Novice"));
	CHECK_EQ(info.requiredExp, int64_t {1000});
	CHECK_EQ(info.percent, 0);

	info = k4::DeriveLevel(500, levels);
	CHECK_EQ(info.name, std::string("Novice"));
	CHECK_EQ(info.percent, 50);

	info = k4::DeriveLevel(1000, levels);
	CHECK_EQ(info.name, std::string("Regular"));
	CHECK_EQ(info.requiredExp, int64_t {5000});
	CHECK_EQ(info.percent, 0);

	info = k4::DeriveLevel(3000, levels);
	CHECK_EQ(info.name, std::string("Regular"));
	CHECK_EQ(info.percent, 50);

	info = k4::DeriveLevel(9000, levels);
	CHECK_EQ(info.name, std::string("Veteran"));
	CHECK_EQ(info.requiredExp, int64_t {5000});
	CHECK_EQ(info.percent, 100);

	info = k4::DeriveLevel(1234, {});
	CHECK_EQ(info.id, 1);
	CHECK(info.name.empty()); // the caller localizes "Level N"/"Уровень N"
	CHECK_EQ(info.currentExp, int64_t {1234});
	CHECK_EQ(info.percent, 100);

	// The live site levels.json: 6 levels, only level 6 has a name.
	const std::vector<k4::ClanLevel> siteLevels = {
		{1, "", 0}, {2, "", 1000}, {3, "", 2000},
		{4, "", 3000}, {5, "", 4000}, {6, "Legend", 5000},
	};
	info = k4::DeriveLevel(0, siteLevels);
	CHECK_EQ(info.id, 1);
	CHECK(info.name.empty());
	CHECK_EQ(info.requiredExp, int64_t {1000});
	CHECK_EQ(info.percent, 0);

	info = k4::DeriveLevel(3000, siteLevels);
	CHECK_EQ(info.id, 4);
	CHECK(info.name.empty());
	CHECK_EQ(info.requiredExp, int64_t {4000});
	CHECK_EQ(info.percent, 0);

	info = k4::DeriveLevel(4500, siteLevels);
	CHECK_EQ(info.id, 5);
	CHECK_EQ(info.percent, 50);

	info = k4::DeriveLevel(9000, siteLevels);
	CHECK_EQ(info.id, 6);
	CHECK_EQ(info.name, std::string("Legend"));
	CHECK_EQ(info.requiredExp, int64_t {5000});
	CHECK_EQ(info.percent, 100);
}

// ---------------------------------------------------------------------------------------------
// kill accounting
// ---------------------------------------------------------------------------------------------

void TestKillDeltas() {
	k4::Config config; // +3 / -2, ignore warmup and same team

	k4::KillDeltas deltas = k4::ComputeKillDeltas(config, true, true, false, false, false, false);
	CHECK(deltas.attacker);
	CHECK_EQ(deltas.attackerDelta, 3);
	CHECK(deltas.victim);
	CHECK_EQ(deltas.victimDelta, -2);

	deltas = k4::ComputeKillDeltas(config, true, true, false, false, false, true);
	CHECK(!deltas.attacker);
	CHECK(!deltas.victim); // warmup: no stats and no bank changes

	config.ignoreWarmup = false;
	deltas = k4::ComputeKillDeltas(config, true, true, false, false, false, true);
	CHECK(deltas.attacker);
	CHECK(deltas.victim);
	config.ignoreWarmup = true;

	deltas = k4::ComputeKillDeltas(config, true, true, true, false, false, false); // suicide
	CHECK(!deltas.attacker);
	CHECK(deltas.victim);
	CHECK_EQ(deltas.victimDelta, -2);

	deltas = k4::ComputeKillDeltas(config, true, true, false, true, false, false); // teamkill
	CHECK(!deltas.attacker);
	CHECK(deltas.victim);

	config.ignoreSameTeam = false;
	deltas = k4::ComputeKillDeltas(config, true, true, false, true, false, false);
	CHECK(deltas.attacker);
	config.ignoreSameTeam = true;

	deltas = k4::ComputeKillDeltas(config, true, true, false, false, true, false); // same clan
	CHECK(deltas.attacker); // ignoreSameClan is off
	config.ignoreSameClan = true;
	deltas = k4::ComputeKillDeltas(config, true, true, false, false, true, false);
	CHECK(!deltas.attacker);
	CHECK(deltas.victim);
	config.ignoreSameClan = false;

	deltas = k4::ComputeKillDeltas(config, false, true, false, false, false, false); // bot/absent attacker
	CHECK(!deltas.attacker);
	CHECK(deltas.victim);

	deltas = k4::ComputeKillDeltas(config, true, false, false, false, false, false); // bot victim
	CHECK(deltas.attacker);
	CHECK(!deltas.victim);

	config.killReward = 0;
	config.deathPenalty = 0;
	deltas = k4::ComputeKillDeltas(config, true, true, false, false, false, false);
	CHECK(deltas.attacker);
	CHECK_EQ(deltas.attackerDelta, 0);
	CHECK(deltas.victim);
	CHECK_EQ(deltas.victimDelta, 0);

	// Bots: by default any death a bot took part in is ignored entirely (DESIGN §4); the
	// optional count_bots=true keeps the old per-participant behaviour.
	CHECK(k4::IgnoreBotDeath(false, true, false));  // human killed a bot
	CHECK(k4::IgnoreBotDeath(false, false, true));  // bot killed a human
	CHECK(k4::IgnoreBotDeath(false, true, true));   // bot vs bot
	CHECK(!k4::IgnoreBotDeath(false, false, false));
	CHECK(!k4::IgnoreBotDeath(true, true, false));
	CHECK(!k4::IgnoreBotDeath(true, false, true));
	CHECK(!k4::IgnoreBotDeath(true, false, false));
}

void TestBankClamp() {
	CHECK_EQ(k4::ClampBankValue(5, 3), 8);
	CHECK_EQ(k4::ClampBankValue(5, -10), 0);
	CHECK_EQ(k4::ClampBankValue(0, -2), 0);
	CHECK_EQ(k4::ClampBankValue(2147483647, 3), 2147483647);
	CHECK_EQ(k4::ClampBankValue(2147483640, 10), 2147483647);

	CHECK(k4::BankDeltaApplies(5, -2));
	CHECK(!k4::BankDeltaApplies(0, -2));
	CHECK(k4::BankDeltaApplies(0, 3));
	CHECK(!k4::BankDeltaApplies(2147483647, 3));
}

void TestSqlBuilders() {
	const std::string add(k4::BankAddSql());
	CHECK(add.find("CASE WHEN bank > 2147483647 - ?") != std::string::npos);
	CHECK(add.find("ELSE bank + ?") != std::string::npos);
	CHECK(add.find("WHERE id = ?") != std::string::npos);

	const std::string subtract(k4::BankSubtractSql());
	CHECK(subtract.find("CASE WHEN bank >= ?") != std::string::npos);
	CHECK(subtract.find("ELSE 0") != std::string::npos);
	CHECK(subtract.find("WHERE id = ?") != std::string::npos);

	CHECK(std::string(k4::KillsIncrementSql()).find("kills = kills + 1") != std::string::npos);
	CHECK(std::string(k4::DeathsIncrementSql()).find("deaths = deaths + 1") != std::string::npos);
	CHECK(std::string(k4::TouchMemberSql()).find("name = ?") != std::string::npos);
	CHECK(std::string(k4::TouchActivitySql()).find("lastactivity = ?") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------------------------

void TestCommands() {
	const std::vector<std::string> tokens = k4::TokenizeCommand("!clan create \"My Clan\" TAG");
	CHECK_EQ(tokens.size(), std::size_t {4});
	CHECK_EQ(tokens[0], std::string("!clan"));
	CHECK_EQ(tokens[1], std::string("create"));
	CHECK_EQ(tokens[2], std::string("My Clan"));
	CHECK_EQ(tokens[3], std::string("TAG"));

	CHECK(k4::ResolveSubcommand("info") == k4::Subcommand::Info);
	CHECK(k4::ResolveSubcommand("STATUS") == k4::Subcommand::Info);
	CHECK(k4::ResolveSubcommand("members") == k4::Subcommand::Members);
	CHECK(k4::ResolveSubcommand("m") == k4::Subcommand::Members);
	CHECK(k4::ResolveSubcommand("top") == k4::Subcommand::Top);
	CHECK(k4::ResolveSubcommand("tops") == k4::Subcommand::Top);
	CHECK(k4::ResolveSubcommand("chat") == k4::Subcommand::Chat);
	CHECK(k4::ResolveSubcommand("gc") == k4::Subcommand::Chat);
	CHECK(k4::ResolveSubcommand("c") == k4::Subcommand::Chat);
	CHECK(k4::ResolveSubcommand("help") == k4::Subcommand::Help);
	CHECK(k4::ResolveSubcommand("?") == k4::Subcommand::Help);
	// Removed site-owned flows are no longer commands.
	CHECK(k4::ResolveSubcommand("create") == k4::Subcommand::Unknown);
	CHECK(k4::ResolveSubcommand("kick") == k4::Subcommand::Unknown);
	CHECK(k4::ResolveSubcommand("upgrade") == k4::Subcommand::Unknown);

	CHECK_EQ(std::string(k4::SubcommandCanonical(k4::Subcommand::Info)), std::string("info"));
	CHECK_EQ(k4::FormatPhrase("{1} [{2}]", {"Clan", "TAG"}), std::string("Clan [TAG]"));
}

// ---------------------------------------------------------------------------------------------
// scoreboard tag (the live "ADM" clan stores its tag as "[ADM]")
// ---------------------------------------------------------------------------------------------

void TestClanTag() {
	// Shipped default: the CS2 scoreboard adds the brackets itself, so the format has none.
	const std::string_view format = "{1}";
	CHECK_EQ(k4::FormatClanTagText(format, "[ADM]"), std::string("ADM"));
	CHECK_EQ(k4::FormatClanTagText(format, "ADM"), std::string("ADM"));
	CHECK_EQ(k4::FormatClanTagText(format, "[A"), std::string("[A")); // no matching pair
	// One matching outer pair is stripped after trimming (and the inside is trimmed too).
	CHECK_EQ(k4::FormatClanTagText(format, " [ADM] "), std::string("ADM"));
	CHECK_EQ(k4::FormatClanTagText(format, "[ ADM ]"), std::string("ADM"));
	CHECK_EQ(k4::FormatClanTagText(format, "{ADM}"), std::string("ADM"));
	CHECK_EQ(k4::FormatClanTagText(format, "(ADM)"), std::string("ADM"));
	CHECK_EQ(k4::FormatClanTagText(format, "<ADM>"), std::string("ADM"));
	CHECK_EQ(k4::FormatClanTagText(format, "[[ADM]]"), std::string("[ADM]")); // exactly one pair
	CHECK_EQ(k4::FormatClanTagText(format, "(ADM]"), std::string("(ADM]"));
	CHECK_EQ(k4::FormatClanTagText(format, "AAA]"), std::string("AAA]"));
	// A custom format is still applied to the unwrapped tag.
	CHECK_EQ(k4::FormatClanTagText("({1})", "ADM"), std::string("(ADM)"));
	CHECK_EQ(k4::FormatClanTagText("({1})", "(ADM)"), std::string("(ADM)"));
	CHECK_EQ(k4::FormatClanTagText(format, "[]"), std::string(""));
	CHECK_EQ(k4::FormatClanTagText(format, ""), std::string(""));
	CHECK_EQ(k4::FormatClanTagText(format, "   "), std::string(""));
}

// ---------------------------------------------------------------------------------------------
// loaded-guild handoff (regression: `guild->id` read after the capture moved the unique_ptr)
// ---------------------------------------------------------------------------------------------

void TestGuildHandoff() {
	auto guild = std::make_unique<k4::Guild>();
	guild->id = 7;
	guild->name = "ADM";
	k4::GuildHandoff handoff = k4::TakeLoadedGuild(std::move(guild));
	CHECK_EQ(handoff.guildId, 7); // read before the shared_ptr took ownership
	CHECK(handoff.guild != nullptr);
	if (handoff.guild) {
		CHECK_EQ(handoff.guild->id, 7);
		CHECK_EQ(handoff.guild->name, std::string("ADM"));
	}
	CHECK(handoff.guild.use_count() == 1);
	CHECK(guild == nullptr); // the unique_ptr was moved into the handoff

	k4::GuildHandoff missing = k4::TakeLoadedGuild(nullptr);
	CHECK_EQ(missing.guildId, 0);
	CHECK(missing.guild == nullptr);
}

// ---------------------------------------------------------------------------------------------
// rights and menu models (Clans::getClanAccessCheck)
// ---------------------------------------------------------------------------------------------

k4::Guild MakeGuildForRights() {
	k4::Guild guild;
	guild.id = 1;
	guild.ownerAuth = "owner";
	guild.slots = 10;
	guild.roleCount = 3;
	guild.type = 2;

	k4::Role ownerRole;
	ownerRole.id = 10;
	ownerRole.clanId = 1;
	ownerRole.name = "Owner";
	ownerRole.type = 1;
	guild.roles.push_back(ownerRole);

	k4::Role officer;
	officer.id = 11;
	officer.clanId = 1;
	officer.name = "Officer";
	officer.type = 3;
	officer.invite = true;
	officer.kick = true;
	officer.roles = true;
	officer.settings = true;
	guild.roles.push_back(officer);

	k4::Role member;
	member.id = 12;
	member.clanId = 1;
	member.name = "Member";
	member.type = 2;
	guild.roles.push_back(member);

	const auto addMember = [&guild](const char* auth, int32_t roleId) {
		k4::Member entry;
		entry.auth = auth;
		entry.clanId = 1;
		entry.roleId = roleId;
		guild.members.push_back(entry);
	};
	addMember("owner", 10);
	addMember("officer", 11);
	addMember("member", 12);
	addMember("norole", 0);
	return guild;
}

void TestRightsAndMenuModels() {
	const k4::Guild guild = MakeGuildForRights();

	// Owner has every right.
	for (const k4::ClanRight right :
		 {k4::ClanRight::Invite, k4::ClanRight::Kick, k4::ClanRight::Requests, k4::ClanRight::Disband,
		  k4::ClanRight::Settings, k4::ClanRight::Roles, k4::ClanRight::Services}) {
		CHECK(k4::HasClanRight(guild, "owner", right));
	}

	// The site maps invite for requests; kick/settings/roles from the columns; disband/services
	// stay owner-only even for an officer.
	CHECK(k4::HasClanRight(guild, "officer", k4::ClanRight::Invite));
	CHECK(k4::HasClanRight(guild, "officer", k4::ClanRight::Requests));
	CHECK(k4::HasClanRight(guild, "officer", k4::ClanRight::Kick));
	CHECK(k4::HasClanRight(guild, "officer", k4::ClanRight::Settings));
	CHECK(k4::HasClanRight(guild, "officer", k4::ClanRight::Roles));
	CHECK(!k4::HasClanRight(guild, "officer", k4::ClanRight::Disband));
	CHECK(!k4::HasClanRight(guild, "officer", k4::ClanRight::Services));

	// Plain member / NULL role / non-member: no rights.
	CHECK(!k4::HasClanRight(guild, "member", k4::ClanRight::Invite));
	CHECK(!k4::HasClanRight(guild, "member", k4::ClanRight::Kick));
	CHECK(!k4::HasClanRight(guild, "norole", k4::ClanRight::Invite));
	CHECK(!k4::HasClanRight(guild, "stranger", k4::ClanRight::Invite));

	k4::MemberMenuModel model = k4::BuildMemberMenuModel(guild, "owner");
	CHECK(model.management);
	CHECK(model.invite && model.kick && model.requests && model.disband);
	CHECK(!model.leave);

	model = k4::BuildMemberMenuModel(guild, "officer");
	CHECK(model.management);
	CHECK(model.invite && model.kick && model.requests);
	CHECK(!model.disband);
	CHECK(model.leave);

	model = k4::BuildMemberMenuModel(guild, "member");
	CHECK(!model.management);
	CHECK(!model.invite && !model.kick && !model.requests && !model.disband);
	CHECK(model.leave);

	// Join action per site type (1 open, 2 closed, 3 requests).
	CHECK(k4::DetermineJoinAction(1, false, false) == k4::JoinAction::JoinOpen);
	CHECK(k4::DetermineJoinAction(2, false, false) == k4::JoinAction::InviteOnly);
	CHECK(k4::DetermineJoinAction(3, false, false) == k4::JoinAction::Apply);
	CHECK(k4::DetermineJoinAction(3, false, true) == k4::JoinAction::CancelRequest);
	CHECK(k4::DetermineJoinAction(1, true, false) == k4::JoinAction::None);
	CHECK(k4::DetermineJoinAction(2, false, true) == k4::JoinAction::InviteOnly);
}

// ---------------------------------------------------------------------------------------------
// invite state machine
// ---------------------------------------------------------------------------------------------

void TestInvites() {
	k4::InviteRegistry invites;
	invites.Configure(60, 10);

	CHECK(invites.Create(1, 100, 200, 1000));
	CHECK(invites.Size() == 1);
	const std::optional<k4::ClanInvite> invite = invites.Get(200, 1005);
	CHECK(invite.has_value());
	CHECK(invite->guildId == 1);
	CHECK(invite->inviter == 100);
	CHECK(invite->target == 200);
	CHECK(invite->expiresAt == 1060);

	// Cooldown for the same inviter.
	CHECK(!invites.Create(1, 100, 300, 1005));
	// Another inviter is fine.
	CHECK(invites.Create(1, 101, 300, 1005));

	// Wrong guild cannot accept.
	CHECK(!invites.Accept(200, 2, 1010));
	CHECK(invites.Get(200, 1010).has_value());
	CHECK(invites.Accept(200, 1, 1010));
	CHECK(!invites.Get(200, 1010).has_value());

	// Expiry.
	CHECK(invites.Create(1, 100, 400, 2000));
	CHECK(!invites.Get(400, 2060).has_value());
	CHECK(!invites.Accept(400, 1, 2060));

	// Disconnect / guild removal.
	CHECK(invites.Create(1, 101, 500, 3000));
	invites.RemoveTarget(500);
	CHECK(!invites.Get(500, 3001).has_value());

	CHECK(invites.Create(7, 102, 600, 4000));
	invites.RemoveGuild(7);
	CHECK(!invites.Get(600, 4001).has_value());

	// Cleanup removes expired invites and stale cooldowns.
	CHECK(invites.Create(1, 103, 700, 5000));
	CHECK(invites.Cleanup(5100) == 1);
	CHECK(invites.Size() == 0);
	CHECK(!invites.CooldownActive(103, 5100));
	CHECK(invites.Create(1, 103, 701, 5100));

	// Accept decision, including the "target joined another clan meanwhile" case.
	k4::InviteRegistry decision;
	decision.Configure(60, 0);
	CHECK(k4::EvaluateInviteAccept(decision, 200, 1, false, true, 1, 10, 1000)
		  == k4::InviteAcceptResult::Expired);
	CHECK(decision.Create(1, 100, 200, 1000));
	CHECK(k4::EvaluateInviteAccept(decision, 200, 1, false, true, 1, 10, 1001)
		  == k4::InviteAcceptResult::Ok);
	CHECK(k4::EvaluateInviteAccept(decision, 200, 1, true, true, 1, 10, 1001)
		  == k4::InviteAcceptResult::AlreadyInClan);
	CHECK(k4::EvaluateInviteAccept(decision, 200, 1, false, false, 1, 10, 1001)
		  == k4::InviteAcceptResult::ClanGone);
	CHECK(k4::EvaluateInviteAccept(decision, 200, 1, false, true, 10, 10, 1001)
		  == k4::InviteAcceptResult::Full);
	CHECK(k4::EvaluateInviteAccept(decision, 200, 2, false, true, 1, 10, 1001)
		  == k4::InviteAcceptResult::Expired);
	CHECK(k4::EvaluateInviteAccept(decision, 200, 1, false, true, 1, 10, 1060)
		  == k4::InviteAcceptResult::Expired);
}

// ---------------------------------------------------------------------------------------------
// action SQL and log JSON
// ---------------------------------------------------------------------------------------------

void TestActionSql() {
	const std::string join = k4::JoinClanSql(true, true);
	CHECK(join.find("FROM DUAL") != std::string::npos);
	CHECK(join.find("m.type = 1") != std::string::npos);
	CHECK(join.find("NOT EXISTS (SELECT 1 FROM clans_players WHERE auth = ?)") != std::string::npos);
	CHECK(join.find("COUNT(*) FROM clans_players p WHERE p.clan_id = ?") != std::string::npos);
	CHECK(join.find("COALESCE(u.slots_up, 0) * ?") != std::string::npos);

	const std::string inviteJoin = k4::JoinClanSql(false, true);
	CHECK(inviteJoin.find("m.type = 1") == std::string::npos);
	CHECK(k4::JoinClanSql(true, false).find("FROM DUAL") == std::string::npos);

	const std::string apply = k4::ApplyRequestSql(true);
	CHECK(apply.find("m.type = 3") != std::string::npos);
	CHECK(apply.find("NOT EXISTS (SELECT 1 FROM clans_request") != std::string::npos);
	CHECK(apply.find("NOT EXISTS (SELECT 1 FROM clans_players") != std::string::npos);

	const std::string leave(k4::LeaveClanSql());
	CHECK(leave.find("auth <> (SELECT owner FROM clans_main") != std::string::npos);

	const std::string kick(k4::KickMemberSql());
	CHECK(kick.find("auth <> (SELECT owner FROM clans_main") != std::string::npos);
	CHECK(kick.find("AND auth <> ?") != std::string::npos);

	const std::string accept(k4::AcceptRequestSql());
	CHECK(accept.find("AND type = 2") != std::string::npos);
	CHECK(accept.find("NOT EXISTS (SELECT 1 FROM clans_players") != std::string::npos);
	CHECK(accept.find("COUNT(*) FROM clans_players p2") != std::string::npos);
	CHECK(accept.find("COALESCE(u.slots_up, 0) * ?") != std::string::npos);
	CHECK(accept.find("FROM clans_request r") != std::string::npos);

	// The accept transaction's delete is guarded by the new membership row: a refused insert
	// (stale request, already a member, full clan) leaves nothing to delete and returns false.
	const std::string acceptDelete(k4::DeleteAcceptedRequestSql());
	CHECK(acceptDelete.find("steamid = ?") != std::string::npos);
	CHECK(acceptDelete.find("EXISTS (SELECT 1 FROM clans_players") != std::string::npos);
	CHECK(acceptDelete.find("clans_request") != std::string::npos);

	// MySQL: row lock first, then the site's table order, the guarded main delete and the check.
	const std::vector<std::string_view> mysqlCascade = k4::DisbandCascadeSql(true, true);
	CHECK_EQ(mysqlCascade.size(), std::size_t {10});
	if (mysqlCascade.size() == 10) {
		CHECK(std::string(mysqlCascade[0]).find("SELECT id FROM clans_main WHERE id = ? FOR UPDATE")
			  != std::string::npos);
		CHECK(std::string(mysqlCascade[1]).find("clans_players") != std::string::npos);
		CHECK(std::string(mysqlCascade[2]).find("clans_roles") != std::string::npos);
		CHECK(std::string(mysqlCascade[3]).find("clans_request") != std::string::npos);
		CHECK(std::string(mysqlCascade[4]).find("clans_reactions") != std::string::npos);
		CHECK(std::string(mysqlCascade[5]).find("clans_views") != std::string::npos);
		CHECK(std::string(mysqlCascade[6]).find("clans_upgrade") != std::string::npos);
		CHECK(std::string(mysqlCascade[7]).find("clans_logs") != std::string::npos);
		CHECK(std::string(mysqlCascade[8]).find("DELETE FROM clans_main WHERE id = ? AND owner = ?")
			  != std::string::npos);
		CHECK(std::string(mysqlCascade[9]).find("remaining") != std::string::npos);
		// Every cascade statement re-checks the owner, so a disband that loses the owner race
		// cannot strip the clan while leaving it in place.
		for (std::size_t index = 1; index + 2 < mysqlCascade.size(); ++index) {
			CHECK(std::string(mysqlCascade[index]).find("m.owner = ?") != std::string::npos);
		}
		for (const std::string_view statement : mysqlCascade) {
			CHECK(std::string(statement).find("DELETE p, r, q") == std::string::npos);
		}
	}

	// delete_logs off (the live site value): logs survive a disband.
	const std::vector<std::string_view> mysqlNoLogs = k4::DisbandCascadeSql(true, false);
	CHECK_EQ(mysqlNoLogs.size(), std::size_t {9});
	for (const std::string_view statement : mysqlNoLogs) {
		CHECK(std::string(statement).find("clans_logs") == std::string::npos);
	}

	// SQLite (tests/dev): same order without FOR UPDATE.
	const std::vector<std::string_view> sqliteCascade = k4::DisbandCascadeSql(false, true);
	CHECK_EQ(sqliteCascade.size(), std::size_t {9});
	if (sqliteCascade.size() == 9) {
		CHECK(std::string(sqliteCascade[0]).find("clans_players") != std::string::npos);
		CHECK(std::string(sqliteCascade[1]).find("clans_roles") != std::string::npos);
		CHECK(std::string(sqliteCascade[2]).find("clans_request") != std::string::npos);
		CHECK(std::string(sqliteCascade[3]).find("clans_reactions") != std::string::npos);
		CHECK(std::string(sqliteCascade[4]).find("clans_views") != std::string::npos);
		CHECK(std::string(sqliteCascade[5]).find("clans_upgrade") != std::string::npos);
		CHECK(std::string(sqliteCascade[6]).find("clans_logs") != std::string::npos);
		CHECK(std::string(sqliteCascade[7]).find("DELETE FROM clans_main WHERE id = ? AND owner = ?")
			  != std::string::npos);
		CHECK(std::string(sqliteCascade[8]).find("remaining") != std::string::npos);
		for (std::size_t index = 0; index + 2 < sqliteCascade.size(); ++index) {
			CHECK(std::string(sqliteCascade[index]).find("m.owner = ?") != std::string::npos);
		}
	}
	const std::vector<std::string_view> sqliteNoLogs = k4::DisbandCascadeSql(false, false);
	CHECK_EQ(sqliteNoLogs.size(), std::size_t {8});
	for (const std::string_view statement : sqliteNoLogs) {
		CHECK(std::string(statement).find("clans_logs") == std::string::npos);
		CHECK(std::string(statement).find("FOR UPDATE") == std::string::npos);
	}
}

void TestLogJson() {
	CHECK_EQ(k4::LogValuePlayer("76561198000000001"),
			 std::string("{\"player\":\"76561198000000001\"}"));
	CHECK_EQ(k4::LogValueKick("76561198000000001", ""),
			 std::string("{\"player\":\"76561198000000001\",\"reason\":\"\"}"));
	CHECK_EQ(k4::JsonEncodePhp("a\"b\\c/d"), std::string("a\\\"b\\\\c\\/d"));
	CHECK_EQ(k4::JsonEncodePhp("line\nnext\t"), std::string("line\\nnext\\t"));
	CHECK_EQ(k4::JsonEncodePhp("\x01"), std::string("\\u0001"));
	CHECK_EQ(k4::JsonEncodePhp("Клан"), std::string("Клан")); // JSON_UNESCAPED_UNICODE
	// PHP still escapes the JS line separators with only JSON_UNESCAPED_UNICODE set.
	CHECK_EQ(k4::JsonEncodePhp("a\xE2\x80\xA8" "b"), std::string("a\\u2028b"));
	CHECK_EQ(k4::JsonEncodePhp("a\xE2\x80\xA9" "b"), std::string("a\\u2029b"));
}

// ---------------------------------------------------------------------------------------------
// store (SQLite always; MariaDB when ./test.sh exports MYSQL_HOST)
// ---------------------------------------------------------------------------------------------

bool CreateSqliteSchema(plgdb::Database& database) {
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

// MariaDB copy of the live clans schema (FKs included: the disband transaction runs the site's
// child-before-parent order). Dropped and recreated so ./test.sh can rerun it.
bool CreateMySqlSchema(plgdb::Database& database) {
	const std::vector<std::string> statements = {
		"DROP TABLE IF EXISTS clans_players",
		"DROP TABLE IF EXISTS clans_request",
		"DROP TABLE IF EXISTS clans_upgrade",
		"DROP TABLE IF EXISTS clans_roles",
		"DROP TABLE IF EXISTS clans_reactions",
		"DROP TABLE IF EXISTS clans_views",
		"DROP TABLE IF EXISTS clans_logs",
		"DROP TABLE IF EXISTS clans_main",
		"CREATE TABLE clans_main (id INT NOT NULL AUTO_INCREMENT, "
		"name VARCHAR(64) NOT NULL, tag VARCHAR(16) NOT NULL, `desc` TEXT NOT NULL, "
		"owner VARCHAR(32) NOT NULL, bank INT NOT NULL DEFAULT 0, "
		"slots INT NOT NULL DEFAULT 0, type TINYINT DEFAULT 1, "
		"exp INT NOT NULL DEFAULT 0, role_count INT NOT NULL DEFAULT 0, "
		"created BIGINT NOT NULL DEFAULT 0, lastupdate BIGINT NOT NULL DEFAULT 0, "
		"duration INT NOT NULL DEFAULT 0, PRIMARY KEY (id)) "
		"ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE clans_roles (id INT NOT NULL AUTO_INCREMENT, clan_id INT NOT NULL, "
		"name VARCHAR(64) NOT NULL, type TINYINT NOT NULL DEFAULT 3, "
		"kick TINYINT(1) NOT NULL DEFAULT 0, roles TINYINT(1) NOT NULL DEFAULT 0, "
		"invite TINYINT(1) NOT NULL DEFAULT 0, settings TINYINT(1) NOT NULL DEFAULT 0, "
		"PRIMARY KEY (id), KEY idx_clan_id (clan_id), "
		"CONSTRAINT fk_clans_roles_clan FOREIGN KEY (clan_id) REFERENCES clans_main (id) "
		"ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE clans_players (id INT NOT NULL AUTO_INCREMENT, auth VARCHAR(32) NOT NULL, "
		"name VARCHAR(64) NOT NULL DEFAULT 'unknown', clan_id INT NOT NULL, "
		"kills INT NOT NULL DEFAULT 0, deaths INT NOT NULL DEFAULT 0, "
		"joined BIGINT NOT NULL DEFAULT 0, lastactivity BIGINT NOT NULL DEFAULT 0, "
		"role_id INT DEFAULT NULL, bank BIGINT NOT NULL DEFAULT 0, "
		"PRIMARY KEY (id), KEY idx_clan_id (clan_id), KEY idx_role_id (role_id), "
		"CONSTRAINT fk_clans_players_clan FOREIGN KEY (clan_id) REFERENCES clans_main (id) "
		"ON DELETE CASCADE, "
		"CONSTRAINT fk_clans_players_role FOREIGN KEY (role_id) REFERENCES clans_roles (id) "
		"ON DELETE SET NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE clans_upgrade (clan_id INT NOT NULL, slots_up INT NOT NULL DEFAULT 0, "
		"roles_up INT NOT NULL DEFAULT 0, PRIMARY KEY (clan_id), "
		"CONSTRAINT fk_upg_clan FOREIGN KEY (clan_id) REFERENCES clans_main (id) "
		"ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE clans_request (id INT NOT NULL AUTO_INCREMENT, clan_id INT NOT NULL, "
		"steamid VARCHAR(32) NOT NULL, name VARCHAR(64) NOT NULL DEFAULT '', "
		"date BIGINT NOT NULL DEFAULT 0, PRIMARY KEY (id), KEY idx_clan (clan_id), "
		"KEY idx_steam (steamid), "
		"CONSTRAINT fk_req_clan FOREIGN KEY (clan_id) REFERENCES clans_main (id) "
		"ON DELETE CASCADE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE clans_reactions (id INT NOT NULL AUTO_INCREMENT, clan_id INT NOT NULL, "
		"auth VARCHAR(32) NOT NULL, type TINYINT NOT NULL, PRIMARY KEY (id), "
		"UNIQUE KEY uniq_clan_user (clan_id, auth)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE clans_views (id INT NOT NULL AUTO_INCREMENT, clan_id INT NOT NULL, "
		"auth VARCHAR(32) NOT NULL, created BIGINT NOT NULL, PRIMARY KEY (id), "
		"UNIQUE KEY uniq_view_user (clan_id, auth)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		"CREATE TABLE clans_logs (id INT NOT NULL AUTO_INCREMENT, clan_id INT NOT NULL, "
		"action VARCHAR(32) NOT NULL, value LONGTEXT NOT NULL, "
		"created BIGINT NOT NULL, status INT NOT NULL, PRIMARY KEY (id)) "
		"ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
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

bool CreateSchema(plgdb::Database& database) {
	return database.GetDriver() == plgdb::Driver::MySQL ? CreateMySqlSchema(database)
														: CreateSqliteSchema(database);
}

void RunStoreScenario(std::unique_ptr<plgdb::Database>& database) {
	k4::GuildStore store(*database);

	// One clan: base slots 10, 1 slots purchase (step 10 -> effective 20), role_count 3,
	// 1 roles purchase (step 3 -> effective 6).
	CHECK(database
			  ->QuerySync("INSERT INTO clans_main (id, name, tag, `desc`, owner, bank, slots, type, exp, "
						  "role_count, created, lastupdate, duration) VALUES "
						  "(1, 'Test Clan', 'TST', 'desc', '76561198000000001', 5, 10, 2, 4200, 3, "
						  "100, 200, 0)")
			  .Ok());
	CHECK(database->QuerySync("INSERT INTO clans_upgrade (clan_id, slots_up, roles_up) VALUES (1, 1, 1)")
			  .Ok());
	CHECK(database
			  ->QuerySync("INSERT INTO clans_roles (id, clan_id, name, type, kick, roles, invite, settings) "
						  "VALUES (10, 1, 'Owner', 1, 1, 1, 1, 1)")
			  .Ok());
	CHECK(database
			  ->QuerySync("INSERT INTO clans_roles (id, clan_id, name, type, kick, roles, invite, settings) "
						  "VALUES (11, 1, 'Member', 2, 0, 0, 1, 0)")
			  .Ok());
	CHECK(database
			  ->QuerySync("INSERT INTO clans_players (auth, name, clan_id, kills, deaths, joined, "
						  "lastactivity, role_id) VALUES "
						  "('76561198000000001', 'Owner', 1, 10, 5, 1000, 1000, 10), "
						  "('76561198000000002', 'Member', 1, 4, 9, 1001, 1001, 11)")
			  .Ok());

	bool loaded = false;
	std::unique_ptr<k4::Guild> guild;
	store.LoadGuild(1, [&](std::unique_ptr<k4::Guild> value) {
		guild = std::move(value);
		loaded = true;
	});
	CHECK(Pump(*database, [&] { return loaded; }));
	CHECK(guild != nullptr);
	if (guild) {
		CHECK_EQ(guild->name, std::string("Test Clan"));
		CHECK_EQ(guild->tag, std::string("TST"));
		CHECK_EQ(guild->ownerAuth, std::string("76561198000000001"));
		CHECK_EQ(guild->bank, 5);
		CHECK_EQ(guild->slots, 10);
		CHECK_EQ(guild->slotsUp, 1);
		CHECK_EQ(guild->rolesUp, 1);
		CHECK_EQ(guild->type, 2);
		CHECK_EQ(guild->exp, 4200);
		CHECK_EQ(k4::EffectiveSlots(*guild, 10), 20);
		CHECK_EQ(k4::EffectiveRoles(*guild, 3), 6);
		CHECK_EQ(guild->members.size(), std::size_t {2});
		CHECK_EQ(guild->roles.size(), std::size_t {2});
		CHECK(k4::FindMemberBySteam(*guild, 76561198000000002ULL) != nullptr);
		CHECK(k4::FindRole(*guild, 11) != nullptr);
		CHECK(k4::IsOwner(*guild, "76561198000000001"));
	}

	bool memberLoaded = false;
	bool memberIsClan = false;
	store.LoadGuildForPlayer("76561198000000002", [&](std::unique_ptr<k4::Guild> value) {
		memberIsClan = value != nullptr && value->id == 1;
		memberLoaded = true;
	});
	CHECK(Pump(*database, [&] { return memberLoaded; }));
	CHECK(memberIsClan);

	// Checked loads separate "no clan" (null guild, empty error) from a failed query.
	bool checkedDone = false;
	std::string checkedError = "x";
	std::unique_ptr<k4::Guild> checkedGuild;
	const auto checkedCallback = [&](std::unique_ptr<k4::Guild> value, const std::string& error) {
		checkedGuild = std::move(value);
		checkedError = error;
		checkedDone = true;
	};
	store.LoadGuildForPlayerChecked("76561198000000002", checkedCallback);
	CHECK(Pump(*database, [&] { return checkedDone; }));
	CHECK(checkedGuild != nullptr && checkedGuild->id == 1);
	CHECK(checkedError.empty());

	checkedDone = false;
	store.LoadGuildForPlayerChecked("76561198000000099", checkedCallback);
	CHECK(Pump(*database, [&] { return checkedDone; }));
	CHECK(checkedGuild == nullptr);
	CHECK(checkedError.empty());
	checkedError = "x";

	checkedDone = false;
	store.LoadGuildChecked(999, checkedCallback);
	CHECK(Pump(*database, [&] { return checkedDone; }));
	CHECK(checkedGuild == nullptr);
	CHECK(checkedError.empty());

	int32_t clanId = -1;
	store.LoadClanId("76561198000000099", [&](int32_t value) { clanId = value; });
	CHECK(Pump(*database, [&] { return clanId != -1; }));
	CHECK_EQ(clanId, 0);

	// Bank: subtract clamps at 0, add works, overflow clamps at INT32_MAX.
	store.AddBank(1, -10);
	CHECK(PumpPending(*database));
	CHECK(database->QuerySync("SELECT bank FROM clans_main WHERE id = 1").GetInt(0, 0) == 0);
	store.AddBank(1, 3);
	CHECK(PumpPending(*database));
	CHECK(database->QuerySync("SELECT bank FROM clans_main WHERE id = 1").GetInt(0, 0) == 3);
	store.AddBank(1, 2147483647);
	CHECK(PumpPending(*database));
	CHECK(database->QuerySync("SELECT bank FROM clans_main WHERE id = 1").GetInt(0, 0) == 2147483647);

	// Stats.
	bool statsDone = false;
	store.AddKillStats("76561198000000001", "76561198000000002", [&](bool ok) {
		CHECK(ok);
		statsDone = true;
	});
	CHECK(Pump(*database, [&] { return statsDone; }));
	CHECK(database->QuerySync("SELECT kills FROM clans_players WHERE auth = '76561198000000001'")
			  .GetInt(0, 0) == 11);
	CHECK(database->QuerySync("SELECT deaths FROM clans_players WHERE auth = '76561198000000002'")
			  .GetInt(0, 0) == 10);

	// Profile refresh.
	bool touched = false;
	store.TouchMember("76561198000000002", "Renamed", [&] { touched = true; });
	CHECK(Pump(*database, [&] { return touched; }));
	CHECK_EQ(database->QuerySync("SELECT name FROM clans_players WHERE auth = '76561198000000002'")
				 .GetString(0, 0),
			 std::string("Renamed"));

	// Tops.
	bool topDone = false;
	std::vector<k4::TopEntry> top;
	store.TopKills(10, true, [&](std::vector<k4::TopEntry> entries) {
		top = std::move(entries);
		topDone = true;
	});
	CHECK(Pump(*database, [&] { return topDone; }));
	CHECK_EQ(top.size(), std::size_t {1});
	if (!top.empty()) {
		CHECK_EQ(top.front().clanId, 1);
		// owner 5 deaths + member 9 + the one from AddKillStats
		CHECK_EQ(static_cast<int64_t>(top.front().value), int64_t {15});
	}

	topDone = false;
	store.TopBank(10, [&](std::vector<k4::TopEntry> entries) {
		top = std::move(entries);
		topDone = true;
	});
	CHECK(Pump(*database, [&] { return topDone; }));
	CHECK_EQ(top.size(), std::size_t {1});
	if (!top.empty()) {
		CHECK_EQ(static_cast<int64_t>(top.front().value), int64_t {2147483647});
	}

	// -------------------------------------------------------------------------------------
	// menu actions against the site schema
	// -------------------------------------------------------------------------------------
	CHECK(database
			  ->QuerySync("INSERT INTO clans_main (id, name, tag, `desc`, owner, bank, slots, type, exp, "
						  "role_count, created, lastupdate) VALUES "
						  "(2, 'Open Clan', 'OPN', '', '76561198000000003', 0, 5, 1, 0, 3, 0, 0),"
						  "(3, 'Request Clan', 'REQ', '', '76561198000000004', 0, 5, 3, 0, 3, 0, 0),"
						  "(4, 'Full Clan', 'FULL', '', '76561198000000010', 0, 0, 1, 0, 3, 0, 0)")
			  .Ok());
	CHECK(database
			  ->QuerySync("INSERT INTO clans_roles (id, clan_id, name, type) VALUES "
						  "(20, 2, 'Owner', 1), (21, 2, 'Member', 2), "
						  "(31, 3, 'Owner', 1), (32, 3, 'Member', 2), "
						  "(41, 4, 'Owner', 1), (42, 4, 'Member', 2)")
			  .Ok());
	CHECK(database->QuerySync("INSERT INTO clans_upgrade (clan_id, slots_up, roles_up) VALUES "
							  "(2, 0, 0), (3, 0, 0), (4, 0, 0)")
			  .Ok());

	bool listDone = false;
	std::vector<k4::ClanSummary> clans;
	store.LoadClanList(30, [&](std::vector<k4::ClanSummary> entries) {
		clans = std::move(entries);
		listDone = true;
	});
	CHECK(Pump(*database, [&] { return listDone; }));
	CHECK_EQ(clans.size(), std::size_t {4});
	if (clans.size() == 4) {
		CHECK_EQ(clans[0].id, 1);
		CHECK_EQ(clans[1].id, 2);
		CHECK_EQ(clans[1].type, 1);
		CHECK_EQ(clans[1].slots, 5);
	}

	// Apply / accept / decline.
	bool hasRequest = true;
	store.HasPendingRequest(3, "76561198000000005",
							[&](bool value) { hasRequest = value; });
	CHECK(Pump(*database, [&] { return !hasRequest; }));
	CHECK(!hasRequest);

	bool applied = false;
	store.ApplyToClan(3, "76561198000000005", "Applicant",
					  [&](bool ok) { applied = ok; });
	CHECK(Pump(*database, [&] { return applied; }));
	CHECK(applied);

	hasRequest = false;
	store.HasPendingRequest(3, "76561198000000005", [&](bool value) { hasRequest = value; });
	CHECK(Pump(*database, [&] { return hasRequest; }));
	CHECK(hasRequest);

	bool duplicateApply = true;
	store.ApplyToClan(3, "76561198000000005", "Applicant",
					  [&](bool ok) { duplicateApply = ok; });
	CHECK(Pump(*database, [&] { return !duplicateApply; }));

	bool requestsDone = false;
	std::vector<k4::ClanRequest> requests;
	store.LoadRequests(3, [&](std::vector<k4::ClanRequest> entries) {
		requests = std::move(entries);
		requestsDone = true;
	});
	CHECK(Pump(*database, [&] { return requestsDone; }));
	CHECK_EQ(requests.size(), std::size_t {1});
	int64_t requestId = requests.empty() ? 0 : requests.front().id;

	bool accepted = false;
	store.AcceptRequest(3, requestId, "76561198000000005", 10, [&](bool ok) { accepted = ok; });
	CHECK(Pump(*database, [&] { return accepted; }));
	CHECK(accepted);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_request WHERE clan_id = 3 AND "
							  "steamid = '76561198000000005'")
			  .GetInt(0, 0) == 0);
	CHECK(database->QuerySync("SELECT role_id FROM clans_players WHERE clan_id = 3 AND "
							  "auth = '76561198000000005'")
			  .GetInt(0, 0) == 32);

	// A refused accept (full clan) must not join the player and must keep the request: the
	// guarded delete runs only when the transaction really created the membership.
	CHECK(database->QuerySync("INSERT INTO clans_request (clan_id, steamid, name, date) VALUES "
							  "(4, '76561198000000013', 'Hopeful', 0)").Ok());
	const int64_t fullRequestId =
		database->QuerySync("SELECT id FROM clans_request WHERE clan_id = 4 AND steamid = "
							"'76561198000000013'").GetInt(0, 0);
	bool fullAccepted = true;
	store.AcceptRequest(4, fullRequestId, "76561198000000013", 10,
						[&](bool ok) { fullAccepted = ok; });
	CHECK(Pump(*database, [&] { return !fullAccepted; }));
	CHECK(!fullAccepted);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_players WHERE auth = '76561198000000013'")
			  .GetInt(0, 0) == 0);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_request WHERE id = " +
							  std::to_string(fullRequestId)).GetInt(0, 0) == 1);

	// A request that no longer exists is refused as well.
	bool missingAccepted = true;
	store.AcceptRequest(3, 999999, "76561198000000014", 10,
						[&](bool ok) { missingAccepted = ok; });
	CHECK(Pump(*database, [&] { return !missingAccepted; }));
	CHECK(!missingAccepted);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_players WHERE auth = '76561198000000014'")
			  .GetInt(0, 0) == 0);

	bool secondApplied = false;
	store.ApplyToClan(3, "76561198000000006", "Decliner",
					  [&](bool ok) { secondApplied = ok; });
	CHECK(Pump(*database, [&] { return secondApplied; }));
	CHECK(secondApplied);
	bool declined = false;
	store.DeclineRequest(3, "76561198000000006", [&](bool ok) { declined = ok; });
	CHECK(Pump(*database, [&] { return declined; }));
	CHECK(declined);

	// Join: type guard, duplicate guard, full guard, invite path.
	bool joined = false;
	store.JoinClan(2, "76561198000000007", "Joiner", 10, true, [&](bool ok) { joined = ok; });
	CHECK(Pump(*database, [&] { return joined; }));
	CHECK(joined);
	bool duplicateJoin = true;
	store.JoinClan(2, "76561198000000007", "Joiner", 10, true,
				   [&](bool ok) { duplicateJoin = ok; });
	CHECK(Pump(*database, [&] { return !duplicateJoin; }));
	CHECK(!duplicateJoin);
	bool wrongType = true;
	store.JoinClan(1, "76561198000000008", "WrongType", 10, true,
				   [&](bool ok) { wrongType = ok; });
	CHECK(Pump(*database, [&] { return !wrongType; }));
	CHECK(!wrongType);
	bool full = true;
	store.JoinClan(4, "76561198000000009", "Full", 10, true, [&](bool ok) { full = ok; });
	CHECK(Pump(*database, [&] { return !full; }));
	CHECK(!full);
	bool invited = false;
	store.JoinClan(2, "76561198000000011", "Invited", 10, false,
				   [&](bool ok) { invited = ok; });
	CHECK(Pump(*database, [&] { return invited; }));
	CHECK(invited);

	// Leave: owner guard, then a normal member; kick guard.
	bool ownerLeft = true;
	store.LeaveClan(2, "76561198000000003", [&](bool ok) { ownerLeft = ok; });
	CHECK(Pump(*database, [&] { return !ownerLeft; }));
	CHECK(!ownerLeft);
	bool left = false;
	store.LeaveClan(2, "76561198000000007", [&](bool ok) { left = ok; });
	CHECK(Pump(*database, [&] { return left; }));
	CHECK(left);
	bool kickedOwner = true;
	store.KickMember(2, "76561198000000003", "76561198000000003",
					 [&](bool ok) { kickedOwner = ok; });
	CHECK(Pump(*database, [&] { return !kickedOwner; }));
	CHECK(!kickedOwner);
	bool kicked = false;
	store.KickMember(2, "76561198000000003", "76561198000000011",
					 [&](bool ok) { kicked = ok; });
	CHECK(Pump(*database, [&] { return kicked; }));
	CHECK(kicked);

	// Logs mirror Clans::addLog.
	bool logged = false;
	store.WriteLog(2, "kick", k4::LogValueKick("76561198000000011", ""),
				   [&](bool ok) { logged = ok; });
	CHECK(Pump(*database, [&] { return logged; }));
	CHECK(logged);
	CHECK_EQ(database->QuerySync("SELECT action FROM clans_logs WHERE clan_id = 2").GetString(0, 0),
			 std::string("kick"));
	CHECK_EQ(database->QuerySync("SELECT value FROM clans_logs WHERE clan_id = 2").GetString(0, 0),
			 std::string("{\"player\":\"76561198000000011\",\"reason\":\"\"}"));
	CHECK(database->QuerySync("SELECT status FROM clans_logs WHERE clan_id = 2").GetInt(0, 0) == 1);

	// Disband: wrong owner leaves everything; the owner removes main + reactions/views/logs.
	// One member left in the clan so the guarded player cascade is exercised.
	CHECK(database
			  ->QuerySync("INSERT INTO clans_players (auth, name, clan_id, kills, deaths, joined, "
						  "lastactivity) VALUES ('76561198000000012', 'Kept', 2, 0, 0, 0, 0)")
			  .Ok());
	CHECK(database->QuerySync("INSERT INTO clans_reactions (clan_id, auth, type) VALUES (2, 'x', 1)")
			  .Ok());
	CHECK(database->QuerySync("INSERT INTO clans_views (clan_id, auth, created) VALUES (2, 'x', 0)")
			  .Ok());
	bool wrongOwnerDisband = true;
	store.DisbandClan(2, "76561198000000099", true,
					  [&](bool ok) { wrongOwnerDisband = ok; });
	CHECK(Pump(*database, [&] { return !wrongOwnerDisband; }));
	CHECK(!wrongOwnerDisband);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_main WHERE id = 2").GetInt(0, 0) == 1);
	// A failed owner guard must not strip the clan: every cascade delete is guarded too.
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_players WHERE clan_id = 2").GetInt(0, 0)
		  == 1);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_roles WHERE clan_id = 2").GetInt(0, 0) == 2);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_reactions WHERE clan_id = 2").GetInt(0, 0)
		  == 1);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_views WHERE clan_id = 2").GetInt(0, 0) == 1);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_upgrade WHERE clan_id = 2").GetInt(0, 0)
		  == 1);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_logs WHERE clan_id = 2").GetInt(0, 0) == 1);
	bool disbanded = false;
	store.DisbandClan(2, "76561198000000003", true, [&](bool ok) { disbanded = ok; });
	CHECK(Pump(*database, [&] { return disbanded; }));
	CHECK(disbanded);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_main WHERE id = 2").GetInt(0, 0) == 0);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_players WHERE clan_id = 2").GetInt(0, 0)
		  == 0);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_roles WHERE clan_id = 2").GetInt(0, 0) == 0);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_reactions WHERE clan_id = 2")
			  .GetInt(0, 0) == 0);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_views WHERE clan_id = 2").GetInt(0, 0)
		  == 0);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_logs WHERE clan_id = 2").GetInt(0, 0)
		  == 0);

	// delete_logs off (live value): the clan's logs must survive the disband.
	bool logKeptDone = false;
	store.WriteLog(4, "kick", k4::LogValueKick("76561198000000009", ""),
				   [&](bool ok) { logKeptDone = ok; });
	CHECK(Pump(*database, [&] { return logKeptDone; }));
	bool disbandKeep = false;
	store.DisbandClan(4, "76561198000000010", false, [&](bool ok) { disbandKeep = ok; });
	CHECK(Pump(*database, [&] { return disbandKeep; }));
	CHECK(disbandKeep);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_main WHERE id = 4").GetInt(0, 0) == 0);
	CHECK(database->QuerySync("SELECT COUNT(*) FROM clans_logs WHERE clan_id = 4").GetInt(0, 0)
		  == 1);

}

// An empty in-memory database: every query fails. The checked loaders must report the failure
// instead of the "no clan" answer that the plugin's membership cache treats as final.
void TestStoreErrorReporting() {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	std::unique_ptr<plgdb::Database> database = plgdb::Database::Create(config);
	CHECK(database != nullptr);
	if (!database) {
		return;
	}
	k4::GuildStore store(*database);

	bool done = false;
	int32_t clanId = -1;
	std::string error;
	store.LoadClanIdChecked("76561198000000001",
							[&](int32_t value, const std::string& message) {
								clanId = value;
								error = message;
								done = true;
							});
	CHECK(Pump(*database, [&] { return done; }));
	CHECK_EQ(clanId, 0);
	CHECK(!error.empty());

	done = false;
	error.clear();
	store.LoadGuildForPlayerChecked(
		"76561198000000001",
		[&](std::unique_ptr<k4::Guild> guild, const std::string& message) {
			CHECK(guild == nullptr);
			error = message;
			done = true;
		});
	CHECK(Pump(*database, [&] { return done; }));
	CHECK(!error.empty());

	done = false;
	error.clear();
	store.LoadGuildChecked(1, [&](std::unique_ptr<k4::Guild> guild, const std::string& message) {
		CHECK(guild == nullptr);
		error = message;
		done = true;
	});
	CHECK(Pump(*database, [&] { return done; }));
	CHECK(!error.empty());
}

void TestStoreAgainst(plgdb::Config config) {
	std::unique_ptr<plgdb::Database> database = plgdb::Database::Create(config);
	CHECK(database != nullptr);
	if (!database) {
		return;
	}
	CHECK(CreateSchema(*database));
	RunStoreScenario(database);
}

void TestStore() {
	const std::string path = "/tmp/k4_guilds_tests_" + std::to_string(::getpid()) + ".sqlite3";
	std::remove(path.c_str());
	plgdb::Config sqlite;
	sqlite.driver = plgdb::Driver::SQLite;
	sqlite.sqlitePath = path;
	TestStoreAgainst(sqlite);
	std::remove(path.c_str());

	// ./test.sh exports the throwaway MariaDB; the same statements then run on MySQL too, which
	// is the driver the production database uses (the disband transaction starts with the row
	// lock and runs the site's child-before-parent delete order).
	const char* host = std::getenv("MYSQL_HOST");
	if (host == nullptr || *host == '\0') {
		return;
	}
	plgdb::Config mysql;
	mysql.driver = plgdb::Driver::MySQL;
	mysql.host = host;
	mysql.port = static_cast<uint16_t>(
		std::atoi(std::getenv("MYSQL_PORT") ? std::getenv("MYSQL_PORT") : "3306"));
	mysql.user = std::getenv("MYSQL_USER") ? std::getenv("MYSQL_USER") : "";
	mysql.password = std::getenv("MYSQL_PASSWORD") ? std::getenv("MYSQL_PASSWORD") : "";
	mysql.database = std::getenv("MYSQL_DATABASE") ? std::getenv("MYSQL_DATABASE") : "plgtest";
	std::printf("store: MariaDB leg\n");
	TestStoreAgainst(mysql);
}


// ---------------------------------------------------------------------------------------------
// KV1 reader (src/kv1.cpp) and the pisexcompat file semantics (src/k4_files.cpp)
// ---------------------------------------------------------------------------------------------

void TestKv1Reader() {
	auto root = k4::ParseKv1("\xEF\xBB\xBF\"Root\"\n{\n"
							 "  // line comment\n"
							 "  \"a\" \"1\" /* block comment */\n"
							 "  \"b\" { \"c\" \"d\" }\n"
							 "  \"e\" unquoted\n"
							 "  \"#base\" \"ignored\"\n"
							 "}\n");
	CHECK(root.has_value());
	if (!root) {
		return;
	}
	CHECK_EQ(std::string(root->name), std::string("Root")); // single top-level block is unwrapped
	CHECK_EQ(root->GetInt("a", 0), 1);
	CHECK_EQ(std::string(root->GetString("e", "")), std::string("unquoted"));
	const k4::KvNode* b = root->Find("b");
	CHECK(b != nullptr && b->isBlock);
	CHECK(b != nullptr && std::string(b->GetString("c", "")) == "d");
	// Case-insensitive key lookup (Valve's FindKey).
	CHECK(std::string(root->GetString("A", "")) == "1");

	auto nested = k4::ParseKv1("Outer { Inner { \"k\" \"v\" } }");
	CHECK(nested.has_value());
	CHECK(nested && nested->Find("inner") != nullptr);
	CHECK(nested && std::string(nested->Find("inner")->GetString("k", "")) == "v");

	// A malformed file is rejected as a whole, never half-read.
	CHECK(!k4::ParseKv1("\"A\" { \"b\" \"c\"").has_value());      // missing }
	CHECK(!k4::ParseKv1("\"a\" \"unterminated").has_value());      // unterminated string
	CHECK(!k4::ParseKv1("\"a\" { } }").has_value());               // stray top-level }
	CHECK(!k4::ParseKv1("\"a\" { /* open").has_value());           // unterminated block comment
}

void TestFilesAndPhrases() {
	const std::string base = "/tmp/k4_guilds_files_" + std::to_string(::getpid());
	std::error_code ec;
	std::filesystem::remove_all(base, ec);
	const std::string configs = base + "/configs";
	const std::string data = base + "/data";
	std::filesystem::create_directories(configs + "/k4_guilds/translations", ec);
	std::filesystem::create_directories(data + "/k4_guilds", ec);

	CHECK_EQ(k4::JoinPath("/a", "b"), std::string("/a/b"));
	CHECK_EQ(k4::JoinPath("/a/", "/b"), std::string("/a/b"));
	CHECK_EQ(k4::JoinPath("", "b"), std::string("b"));

	// databases.cfg: the plugin config dir is used when it exists (the raw Metamod spelling is
	// absent in this fixture); the data dir is only a later fallback.
	const std::string configDb = configs + "/k4_guilds/databases.cfg";
	{
		std::ofstream out(configDb, std::ios::binary | std::ios::trunc);
		out << "\"Databases\" { \"k4_guilds\" { \"driver\" \"sqlite\" } }\n";
	}
	CHECK_EQ(k4::ResolveConfigPath("addons/configs/databases.cfg", configs, data, "k4_guilds"),
			 configDb);
	CHECK_EQ(k4::ResolveDataPath("k4_guilds.sqlite3", data, "k4_guilds"),
			 data + "/k4_guilds/k4_guilds.sqlite3");
	CHECK_EQ(k4::ResolveDataPath("addons/data/k4_guilds.sqlite3", data, "k4_guilds"),
			 data + "/k4_guilds/k4_guilds.sqlite3");

	// phrases: <configs>/<plugin>/translations/<file> wins over the config dir copy.
	const std::string phrasePath = configs + "/k4_guilds/translations/k4_guilds.phrases.txt";
	{
		std::ofstream out(phrasePath, std::ios::binary | std::ios::trunc);
		out << "\"Phrases\" { }\n";
	}
	CHECK_EQ(k4::ResolveTranslationsPath("k4_guilds.phrases.txt", configs, data, "k4_guilds"),
			 phrasePath);
	// Pisex's LoadTranslations() appends ".txt" to a name without it.
	CHECK_EQ(k4::ResolveTranslationsPath("k4_guilds.phrases", configs, data, "k4_guilds"),
			 phrasePath);

	// --- language chain ---------------------------------------------------------------------
	CHECK_EQ(k4::NormalizeLanguage("russian"), std::string("ru"));
	CHECK_EQ(k4::NormalizeLanguage("English"), std::string("en"));
	CHECK_EQ(k4::NormalizeLanguage("ukrainian"), std::string("ua"));
	CHECK_EQ(k4::NormalizeLanguage("schinese"), std::string("zh-CN"));
	CHECK_EQ(k4::NormalizeLanguage("klingon"), std::string("klingon"));
	CHECK_EQ(k4::DetectLanguage(std::string("\"Core\" { \"ServerLang\" \"russian\" }"), "english"),
			 std::string("ru"));
	CHECK_EQ(k4::DetectLanguage(std::nullopt, "German"), std::string("de"));
	CHECK_EQ(k4::DetectLanguage(std::nullopt, ""), std::string("en"));
	CHECK_EQ(k4::DetectLanguage(std::string("\"Core\" { }"), "Turkish"), std::string("tr"));
	CHECK_EQ(k4::DetectLanguage(std::string("not keyvalues {"), ""), std::string("en"));

	// --- phrase table -----------------------------------------------------------------------
	k4::PhraseMap phrases;
	CHECK(k4::LoadPhrases("\"Phrases\" {"
						  "  \"OnlyEn\" { \"en\" \"english\" }"
						  "  \"Both\"   { \"en\" \"en-text\" \"ru\" \"ru-text\" }"
						  "  \"OnlyDe\" { \"de\" \"de-text\" }"
						  "}",
						  "ru", phrases));
	CHECK_EQ(k4::GetPhrase(phrases, "Both"), std::string("ru-text"));
	CHECK_EQ(k4::GetPhrase(phrases, "OnlyEn"), std::string("english")); // en fallback
	CHECK_EQ(k4::GetPhrase(phrases, "OnlyDe"), std::string("de-text")); // any-language fallback
	CHECK_EQ(k4::GetPhrase(phrases, "Missing"), std::string(""));
	CHECK(!k4::LoadPhrases("\"Phrases\" { \"A\" { \"en\" \"x\"", "en", phrases));
	CHECK_EQ(k4::GetPhrase(phrases, "Both"), std::string("ru-text")); // untouched on failure

	// The shipped file: both languages complete, no empty text.
	const std::string shipped = ReadFile(K4_GUILDS_TEST_PHRASES);
	CHECK(!shipped.empty());
	k4::PhraseMap ru;
	k4::PhraseMap en;
	CHECK(k4::LoadPhrases(shipped, "ru", ru));
	CHECK(k4::LoadPhrases(shipped, "en", en));
	CHECK(!ru.empty());
	CHECK_EQ(ru.size(), en.size());
	bool allFilled = true;
	for (const auto& [key, text] : ru) {
		if (text.empty() || en[key].empty()) {
			allFilled = false;
		}
	}
	CHECK(allFilled);

	// --- colour table ------------------------------------------------------------------------
	CHECK_EQ(k4::Colorize("{LIME}x\\n{GRAY}y"),
			 std::string("\x05" "x\xe2\x80\xa9" "\x08" "y"));
	CHECK_EQ(k4::StripColorTags("{LIME}x\\n{GRAY}y"), std::string("x\ny"));
	CHECK_EQ(k4::StripColorTags("no tags {UNKNOWN}"), std::string("no tags {UNKNOWN}"));
	CHECK_EQ(k4::Colorize("{DEFAULT}"), std::string("\x01"));
	CHECK_EQ(k4::Colorize("{GRAYBLUE}"), std::string("\x0A"));

	// --- databases.cfg ----------------------------------------------------------------------
	const k4::DatabaseConfigResult parsed = k4::ParseDatabaseConfig(
		"\"Databases\"\n{\n"
		"  \"k4_guilds\"\n  {\n"
		"    \"driver\" \"sqlite\"\n"
		"    \"host\" \"db.local\"\n"
		"    \"port\" \"3307\"\n"
		"    \"user\" \"u\"\n"
		"    \"pass\" \"p\"\n"
		"    \"database\" \"clans\"\n"
		"    \"sqlite_path\" \"x.sqlite3\"\n"
		"  }\n"
		"}\n",
		"k4_guilds");
	CHECK(parsed.found);
	CHECK(parsed.error.empty());
	CHECK_EQ(parsed.settings.driver, std::string("sqlite"));
	CHECK_EQ(parsed.settings.host, std::string("db.local"));
	CHECK_EQ(parsed.settings.port, 3307);
	CHECK_EQ(parsed.settings.user, std::string("u"));
	CHECK_EQ(parsed.settings.password, std::string("p"));
	CHECK_EQ(parsed.settings.database, std::string("clans"));
	CHECK_EQ(parsed.settings.sqlitePath, std::string("x.sqlite3"));
	// Driver default and a missing section.
	CHECK_EQ(k4::ParseDatabaseConfig("\"Databases\" { \"k4_guilds\" { } }", "k4_guilds").settings.driver,
			 std::string("mysql"));
	CHECK(!k4::ParseDatabaseConfig("\"Databases\" { \"other\" { } }", "k4_guilds").found);
	CHECK(!k4::ParseDatabaseConfig("\"Databases\" { \"k4_guilds\" {", "k4_guilds").error.empty());

	// The shipped template parses and carries no credentials.
	const std::string templatePath = std::string(K4_GUILDS_TEST_DATA_DIR) + "/databases.cfg.example";
	const std::string templateText = ReadFile(templatePath);
	CHECK(!templateText.empty());
	const k4::DatabaseConfigResult shippedDb = k4::ParseDatabaseConfig(templateText, "k4_guilds");
	CHECK(shippedDb.found);
	CHECK(shippedDb.settings.user.empty());
	CHECK(shippedDb.settings.password.empty());

	std::filesystem::remove_all(base, ec);
}

void TestS2sdkHelpers() {
	// PITFALLS C1: only 0 (world) and -1 (invalid) are not entity handles.
	CHECK(!k4::IsValidEntityHandle(0));
	CHECK(!k4::IsValidEntityHandle(-1));
	CHECK(k4::IsValidEntityHandle(1));
	CHECK(k4::IsValidEntityHandle(-123456)); // serial bit 31 set: a valid CEntityHandle

	// pisexcompat ResolveS2sdkMenuTypeName(), HUD_LAYOUT default, table preserved exactly.
	CHECK_EQ(k4::ResolveMenuTypeName("chat", true, true), std::string("chat"));
	CHECK_EQ(k4::ResolveMenuTypeName("centerhtml", true, true), std::string("centerhtml"));
	CHECK_EQ(k4::ResolveMenuTypeName("button", true, true), std::string("button"));
	CHECK_EQ(k4::ResolveMenuTypeName("hud", true, true), std::string("hud"));
	CHECK_EQ(k4::ResolveMenuTypeName("hud", false, true), std::string("button"));
	CHECK_EQ(k4::ResolveMenuTypeName("wasd", true, true), std::string("wasd"));
	CHECK_EQ(k4::ResolveMenuTypeName("wasd", true, false), std::string("button"));
	CHECK_EQ(k4::ResolveMenuTypeName("", true, true), std::string("hud"));
	CHECK_EQ(k4::ResolveMenuTypeName("", false, true), std::string("button")); // never wasd
	CHECK_EQ(k4::ResolveMenuTypeName("legacy", true, false), std::string("hud"));
	CHECK_EQ(k4::ResolveMenuTypeName("legacy", false, false), std::string("button"));

	CHECK(k4::IsChatTrigger('!'));
	CHECK(k4::IsChatTrigger('/'));
	CHECK(!k4::IsChatTrigger('.'));
	CHECK_EQ(k4::StripChatTrigger("!clan"), std::string("clan"));
	CHECK_EQ(k4::StripChatTrigger("/clan"), std::string("clan"));
	CHECK_EQ(k4::StripChatTrigger(".clan"), std::string(".clan"));
	CHECK_EQ(k4::StripChatTrigger("clan"), std::string("clan"));
}

} // namespace

int main() {
	TestConfig();
	TestLevels();
	TestKillDeltas();
	TestBankClamp();
	TestSqlBuilders();
	TestCommands();
	TestClanTag();
	TestGuildHandoff();
	TestRightsAndMenuModels();
	TestInvites();
	TestActionSql();
	TestLogJson();
	TestStoreErrorReporting();
	TestStore();
	TestKv1Reader();
	TestFilesAndPhrases();
	TestS2sdkHelpers();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
