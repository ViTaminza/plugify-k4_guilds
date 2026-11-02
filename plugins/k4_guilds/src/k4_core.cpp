#include "k4_core.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <utility>

#include <nlohmann/json.hpp>

namespace k4 {
namespace {

std::string ReplaceAll(std::string text, std::string_view needle, std::string_view replacement) {
	if (needle.empty()) {
		return text;
	}
	std::string::size_type position = 0;
	while ((position = text.find(needle, position)) != std::string::npos) {
		text.replace(position, needle.size(), replacement);
		position += replacement.size();
	}
	return text;
}

// --- case-insensitive JSON access -------------------------------------------------------------

const nlohmann::json* FindMember(const nlohmann::json& object, std::string_view key) {
	if (!object.is_object()) {
		return nullptr;
	}
	for (auto it = object.begin(); it != object.end(); ++it) {
		if (EqualsIgnoreCase(it.key(), key)) {
			return &it.value();
		}
	}
	return nullptr;
}

const nlohmann::json* FindSection(const nlohmann::json& root, std::string_view key) {
	const nlohmann::json* section = FindMember(root, key);
	return section != nullptr && section->is_object() ? section : nullptr;
}

bool ReadString(const nlohmann::json& object, std::string_view key, std::string& out, std::string& error) {
	const nlohmann::json* value = FindMember(object, key);
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (!value->is_string()) {
		error = "property '" + std::string(key) + "' must be a string";
		return false;
	}
	out = value->get<std::string>();
	return true;
}

bool ReadInt(const nlohmann::json& object, std::string_view key, int& out, std::string& error) {
	const nlohmann::json* value = FindMember(object, key);
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (!value->is_number()) {
		error = "property '" + std::string(key) + "' must be a number";
		return false;
	}
	double number = value->get<double>();
	if (!std::isfinite(number)) {
		error = "property '" + std::string(key) + "' must be finite";
		return false;
	}
	number = std::clamp(number, static_cast<double>(std::numeric_limits<int32_t>::min()),
						static_cast<double>(std::numeric_limits<int32_t>::max()));
	out = static_cast<int>(number);
	return true;
}

bool ReadInt64(const nlohmann::json& object, std::string_view key, int64_t& out, std::string& error) {
	const nlohmann::json* value = FindMember(object, key);
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (value->is_string()) {
		// The live site's levels.json stores the thresholds as strings ("exp": "1000");
		// accept both spellings so a copy of that file works unchanged.
		const std::string& text = value->get_ref<const std::string&>();
		char* end = nullptr;
		errno = 0;
		const long long parsed = std::strtoll(text.c_str(), &end, 10);
		if (text.empty() || end == text.c_str() || *end != '\0' || errno == ERANGE) {
			error = "property '" + std::string(key) + "' must be a number";
			return false;
		}
		out = static_cast<int64_t>(parsed);
		return true;
	}
	if (!value->is_number()) {
		error = "property '" + std::string(key) + "' must be a number";
		return false;
	}
	if (value->is_number_unsigned()) {
		const uint64_t raw = value->get<uint64_t>();
		out = raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
				  ? std::numeric_limits<int64_t>::max()
				  : static_cast<int64_t>(raw);
		return true;
	}
	double number = value->get<double>();
	if (!std::isfinite(number)) {
		error = "property '" + std::string(key) + "' must be finite";
		return false;
	}
	number = std::clamp(number, static_cast<double>(std::numeric_limits<int64_t>::min()),
						static_cast<double>(std::numeric_limits<int64_t>::max()));
	out = static_cast<int64_t>(number);
	return true;
}

bool ReadBool(const nlohmann::json& object, std::string_view key, bool& out, std::string& error) {
	const nlohmann::json* value = FindMember(object, key);
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (!value->is_boolean()) {
		error = "property '" + std::string(key) + "' must be a boolean";
		return false;
	}
	out = value->get<bool>();
	return true;
}

bool ReadStringArray(const nlohmann::json& object, std::string_view key, std::vector<std::string>& out,
					 std::string& error) {
	const nlohmann::json* value = FindMember(object, key);
	if (value == nullptr || value->is_null()) {
		return true;
	}
	if (!value->is_array()) {
		error = "property '" + std::string(key) + "' must be an array of strings";
		return false;
	}
	std::vector<std::string> values;
	for (const nlohmann::json& element : *value) {
		if (!element.is_string()) {
			error = "property '" + std::string(key) + "' must be an array of strings";
			return false;
		}
		values.push_back(element.get<std::string>());
	}
	out = std::move(values);
	return true;
}

bool ParseLevels(const nlohmann::json& root, std::vector<ClanLevel>& out, std::string& error) {
	const nlohmann::json* levels = FindMember(root, "levels");
	if (levels == nullptr || levels->is_null()) {
		return true;
	}
	if (!levels->is_array()) {
		error = "property 'levels' must be an array";
		return false;
	}
	std::vector<ClanLevel> parsed;
	for (const nlohmann::json& entry : *levels) {
		if (!entry.is_object()) {
			error = "every entry of 'levels' must be an object";
			return false;
		}
		ClanLevel level;
		ReadInt(entry, "id", level.id, error);
		ReadString(entry, "name", level.name, error);
		ReadInt64(entry, "exp", level.exp, error);
		parsed.push_back(std::move(level));
	}
	out = std::move(parsed);
	return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------

std::string LowerAscii(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (const char character : text) {
		result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}
	return result;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(left[index]))
			!= std::tolower(static_cast<unsigned char>(right[index]))) {
			return false;
		}
	}
	return true;
}

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
	if (needle.empty()) {
		return true;
	}
	if (needle.size() > haystack.size()) {
		return false;
	}
	return LowerAscii(haystack).find(LowerAscii(needle)) != std::string::npos;
}

std::string TrimSpaces(std::string_view text) {
	const std::size_t begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos) {
		return {};
	}
	const std::size_t end = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(begin, end - begin + 1));
}

std::string FormatPhrase(std::string_view phrase, const std::vector<std::string>& args) {
	std::string result(phrase);
	for (std::size_t index = 0; index < args.size(); ++index) {
		result = ReplaceAll(std::move(result), "{" + std::to_string(index + 1) + "}", args[index]);
	}
	return result;
}

std::string FormatClanTagText(std::string_view format, std::string_view tag) {
	std::string text = TrimSpaces(tag);
	// The CS2 scoreboard already renders m_szClan inside its own brackets, so a site tag that
	// stores a wrapper ("[ADM]") must lose exactly that outer pair before the format is
	// applied - otherwise the player sees "[[ADM]]". Any other wrapper stays part of the tag.
	if (text.size() >= 2) {
		char closer = '\0';
		switch (text.front()) {
		case '[': closer = ']'; break;
		case '(': closer = ')'; break;
		case '{': closer = '}'; break;
		case '<': closer = '>'; break;
		default: break;
		}
		if (closer != '\0' && text.back() == closer) {
			text = TrimSpaces(std::string_view(text).substr(1, text.size() - 2));
		}
	}
	if (text.empty()) {
		return {};
	}
	return FormatPhrase(format, {text});
}

// ---------------------------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------------------------

bool ParseConfig(std::string_view text, Config& out, std::string& error) {
	error.clear();
	nlohmann::json root;
	try {
		root = nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/true,
									 /*ignore_comments=*/true);
	} catch (const std::exception& exception) {
		error = std::string("config.json parse error: ") + exception.what();
		return false;
	}
	if (!root.is_object()) {
		error = "config.json must be a JSON object";
		return false;
	}

	Config config;
	std::string typeError;

	if (const nlohmann::json* section = FindSection(root, "commands")) {
		ReadStringArray(*section, "chat_aliases", config.chatAliases, typeError);
		ReadStringArray(*section, "console_aliases", config.consoleAliases, typeError);
	}
	if (const nlohmann::json* section = FindSection(root, "kill_rewards")) {
		ReadInt(*section, "kill_reward", config.killReward, typeError);
		ReadInt(*section, "death_penalty", config.deathPenalty, typeError);
		ReadBool(*section, "ignore_warmup", config.ignoreWarmup, typeError);
		ReadBool(*section, "ignore_same_team", config.ignoreSameTeam, typeError);
		ReadBool(*section, "ignore_same_clan", config.ignoreSameClan, typeError);
		ReadBool(*section, "count_bots", config.countBots, typeError);
	}
	if (const nlohmann::json* section = FindSection(root, "membership")) {
		ReadInt(*section, "refresh_seconds", config.membershipRefreshSeconds, typeError);
	}
	if (const nlohmann::json* section = FindSection(root, "guild")) {
		ReadBool(*section, "show_tag_on_scoreboard", config.showTagOnScoreboard, typeError);
		ReadInt(*section, "scoreboard_refresh_seconds", config.scoreboardRefreshSeconds, typeError);
	}
	if (const nlohmann::json* section = FindSection(root, "top")) {
		ReadBool(*section, "enable_bank", config.topBank, typeError);
		ReadBool(*section, "enable_exp", config.topExp, typeError);
		ReadBool(*section, "enable_kills", config.topKills, typeError);
		ReadBool(*section, "enable_deaths", config.topDeaths, typeError);
	}
	if (const nlohmann::json* section = FindSection(root, "services")) {
		ReadInt(*section, "slots_step", config.slotsStep, typeError);
		ReadInt(*section, "roles_step", config.rolesStep, typeError);
	}
	if (const nlohmann::json* section = FindSection(root, "chat")) {
		ReadBool(*section, "guild_chat", config.guildChat, typeError);
	}
	if (const nlohmann::json* section = FindSection(root, "invites")) {
		ReadInt(*section, "expire_seconds", config.inviteExpireSeconds, typeError);
		ReadInt(*section, "cooldown_seconds", config.inviteCooldownSeconds, typeError);
	}
	if (const nlohmann::json* section = FindSection(root, "logs")) {
		ReadBool(*section, "on", config.logs.on, typeError);
		ReadBool(*section, "delete_logs", config.logs.deleteLogs, typeError);
		ReadBool(*section, "join", config.logs.join, typeError);
		ReadBool(*section, "request", config.logs.request, typeError);
		ReadBool(*section, "exit", config.logs.exit, typeError);
		ReadBool(*section, "kick", config.logs.kick, typeError);
		ReadBool(*section, "request_accept", config.logs.requestAccept, typeError);
		ReadBool(*section, "request_cancel", config.logs.requestCancel, typeError);
	}
	ParseLevels(root, config.levels, typeError);

	if (config.killReward < 0) {
		config.killReward = 0;
	}
	if (config.deathPenalty < 0) {
		config.deathPenalty = 0;
	}
	if (config.membershipRefreshSeconds < 5) {
		config.membershipRefreshSeconds = 5;
	}
	if (config.scoreboardRefreshSeconds < 0) {
		config.scoreboardRefreshSeconds = 0;
	}
	if (config.slotsStep < 0) {
		config.slotsStep = 0;
	}
	if (config.rolesStep < 0) {
		config.rolesStep = 0;
	}

	error = std::move(typeError);
	out = std::move(config);
	return true;
}

bool ShouldLogAction(const Config& config, std::string_view action) {
	if (!config.logs.on) {
		return false;
	}
	if (action == "join") {
		return config.logs.join;
	}
	if (action == "request") {
		return config.logs.request;
	}
	if (action == "exit") {
		return config.logs.exit;
	}
	if (action == "kick") {
		return config.logs.kick;
	}
	if (action == "request_accept") {
		return config.logs.requestAccept;
	}
	if (action == "request_cancel") {
		return config.logs.requestCancel;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// live DB model helpers
// ---------------------------------------------------------------------------------------------

const Member* FindMember(const Guild& guild, std::string_view auth) {
	for (const Member& member : guild.members) {
		if (member.auth == auth) {
			return &member;
		}
	}
	return nullptr;
}

const Member* FindMemberBySteam(const Guild& guild, uint64_t steamid) {
	const std::string auth = std::to_string(steamid);
	return FindMember(guild, auth);
}

const Role* FindRole(const Guild& guild, int32_t roleId) {
	if (roleId == 0) {
		return nullptr;
	}
	for (const Role& role : guild.roles) {
		if (role.id == roleId) {
			return &role;
		}
	}
	return nullptr;
}

bool IsOwner(const Guild& guild, std::string_view auth) {
	return !auth.empty() && guild.ownerAuth == auth;
}

GuildHandoff TakeLoadedGuild(std::unique_ptr<Guild> guild) {
	GuildHandoff handoff;
	if (guild) {
		// Read the id while the unique_ptr is still valid: the shared_ptr constructor below
		// moves it (and nulls the source).
		handoff.guildId = guild->id;
	}
	handoff.guild = std::shared_ptr<Guild>(std::move(guild));
	return handoff;
}

const char* GuildTypeName(int type) {
	switch (type) {
	case 1: return "open";
	case 2: return "inviteonly";
	case 3: return "request";
	default: return "unknown";
	}
}

int32_t EffectiveSlots(const Guild& guild, int slotsStep) {
	int64_t slots = guild.slots + static_cast<int64_t>(guild.slotsUp) * slotsStep;
	if (slots < 0) {
		slots = 0;
	}
	return static_cast<int32_t>(std::min<int64_t>(slots, std::numeric_limits<int32_t>::max()));
}

int32_t EffectiveRoles(const Guild& guild, int rolesStep) {
	int64_t roles = guild.roleCount + static_cast<int64_t>(guild.rolesUp) * rolesStep;
	if (roles < 0) {
		roles = 0;
	}
	return static_cast<int32_t>(std::min<int64_t>(roles, std::numeric_limits<int32_t>::max()));
}

// ---------------------------------------------------------------------------------------------
// rights (Clans::getClanAccessCheck)
// ---------------------------------------------------------------------------------------------

bool HasClanRight(const Guild& guild, std::string_view auth, ClanRight right) {
	if (IsOwner(guild, auth)) {
		return true;
	}
	const Member* member = FindMember(guild, auth);
	if (member == nullptr) {
		return false;
	}
	const Role* role = FindRole(guild, member->roleId);
	if (role == nullptr || role->clanId != guild.id) {
		return false;
	}
	switch (right) {
	case ClanRight::Invite: return role->invite;
	case ClanRight::Kick: return role->kick;
	case ClanRight::Requests: return role->invite; // the site uses 'invite' for requests
	case ClanRight::Disband: return false;        // owner only (ClanDelete)
	case ClanRight::Settings: return role->settings;
	case ClanRight::Roles: return role->roles;
	case ClanRight::Services: return false; // owner only
	}
	return false;
}

MemberMenuModel BuildMemberMenuModel(const Guild& guild, std::string_view auth) {
	MemberMenuModel model;
	if (IsOwner(guild, auth)) {
		model.management = true;
		model.invite = true;
		model.kick = true;
		model.requests = true;
		model.disband = true;
		model.leave = false;
		return model;
	}
	if (FindMember(guild, auth) == nullptr) {
		model.info = false;
		model.members = false;
		model.top = false;
		return model;
	}
	model.invite = HasClanRight(guild, auth, ClanRight::Invite);
	model.kick = HasClanRight(guild, auth, ClanRight::Kick);
	model.requests = HasClanRight(guild, auth, ClanRight::Requests);
	model.disband = HasClanRight(guild, auth, ClanRight::Disband);
	model.management = model.invite || model.kick || model.requests || model.disband;
	model.leave = true;
	return model;
}

JoinAction DetermineJoinAction(int type, bool inClan, bool hasPendingRequest) {
	if (inClan) {
		return JoinAction::None;
	}
	if (type == 1) {
		return JoinAction::JoinOpen;
	}
	if (type == 3) {
		return hasPendingRequest ? JoinAction::CancelRequest : JoinAction::Apply;
	}
	return JoinAction::InviteOnly;
}

// ---------------------------------------------------------------------------------------------
// invites
// ---------------------------------------------------------------------------------------------

void InviteRegistry::Configure(int expireSeconds, int cooldownSeconds) {
	m_expireSeconds = expireSeconds > 0 ? expireSeconds : 60;
	m_cooldownSeconds = cooldownSeconds >= 0 ? cooldownSeconds : 0;
}

bool InviteRegistry::CooldownActive(uint64_t inviter, int64_t now) const {
	if (m_cooldownSeconds <= 0 || inviter == 0) {
		return false;
	}
	const auto entry = m_lastInvite.find(inviter);
	if (entry == m_lastInvite.end()) {
		return false;
	}
	return now - entry->second < m_cooldownSeconds;
}

bool InviteRegistry::Create(int32_t guildId, uint64_t inviter, uint64_t target, int64_t now) {
	if (target == 0 || inviter == 0 || guildId <= 0 || CooldownActive(inviter, now)) {
		return false;
	}
	Cleanup(now);
	ClanInvite invite;
	invite.guildId = guildId;
	invite.inviter = inviter;
	invite.target = target;
	invite.expiresAt = now + m_expireSeconds;
	m_invites[target] = invite;
	m_lastInvite[inviter] = now;
	return true;
}

std::optional<ClanInvite> InviteRegistry::Get(uint64_t target, int64_t now) {
	const auto entry = m_invites.find(target);
	if (entry == m_invites.end()) {
		return std::nullopt;
	}
	if (entry->second.expiresAt <= now) {
		m_invites.erase(entry);
		return std::nullopt;
	}
	return entry->second;
}

bool InviteRegistry::Accept(uint64_t target, int32_t guildId, int64_t now) {
	const std::optional<ClanInvite> invite = Get(target, now);
	if (!invite.has_value() || invite->guildId != guildId) {
		return false;
	}
	m_invites.erase(target);
	return true;
}

void InviteRegistry::Decline(uint64_t target) {
	m_invites.erase(target);
}

void InviteRegistry::RemoveTarget(uint64_t target) {
	m_invites.erase(target);
}

void InviteRegistry::RemoveGuild(int32_t guildId) {
	for (auto it = m_invites.begin(); it != m_invites.end();) {
		if (it->second.guildId == guildId) {
			it = m_invites.erase(it);
		} else {
			++it;
		}
	}
}

InviteAcceptResult EvaluateInviteAccept(InviteRegistry& invites, uint64_t target, int32_t guildId,
										bool targetInClan, bool clanExists, int32_t memberCount,
										int32_t effectiveSlots, int64_t now) {
	const std::optional<ClanInvite> invite = invites.Get(target, now);
	if (!invite.has_value() || invite->guildId != guildId || invite->expiresAt <= now) {
		return InviteAcceptResult::Expired;
	}
	if (!clanExists) {
		return InviteAcceptResult::ClanGone;
	}
	if (targetInClan) {
		return InviteAcceptResult::AlreadyInClan; // the site accepted them meanwhile
	}
	if (memberCount >= effectiveSlots) {
		return InviteAcceptResult::Full;
	}
	return InviteAcceptResult::Ok;
}

std::size_t InviteRegistry::Cleanup(int64_t now) {
	std::size_t removed = 0;
	for (auto it = m_invites.begin(); it != m_invites.end();) {
		if (it->second.expiresAt <= now) {
			it = m_invites.erase(it);
			++removed;
		} else {
			++it;
		}
	}
	for (auto it = m_lastInvite.begin(); it != m_lastInvite.end();) {
		if (m_cooldownSeconds <= 0 || now - it->second >= m_cooldownSeconds) {
			it = m_lastInvite.erase(it);
		} else {
			++it;
		}
	}
	return removed;
}

// ---------------------------------------------------------------------------------------------
// levels (Clans::getClanLevelProgress)
// ---------------------------------------------------------------------------------------------

LevelInfo DeriveLevel(int64_t exp, const std::vector<ClanLevel>& levels) {
	LevelInfo info;
	info.currentExp = exp;
	info.requiredExp = exp;
	info.percent = 100;
	if (levels.empty()) {
		info.id = 1; // the site's empty-levels fallback: "Level 1"/"Уровень 1"
		return info;
	}
	std::vector<ClanLevel> sorted = levels;
	std::sort(sorted.begin(), sorted.end(),
			  [](const ClanLevel& left, const ClanLevel& right) { return left.exp < right.exp; });
	const ClanLevel* current = &sorted.front();
	const ClanLevel* next = nullptr;
	for (const ClanLevel& level : sorted) {
		if (exp >= level.exp) {
			current = &level;
		} else {
			next = &level;
			break;
		}
	}
	info.id = current->id;
	info.name = current->name; // empty name: the caller localizes "Level N"/"Уровень N"
	info.requiredExp = next != nullptr ? next->exp : current->exp;
	const int64_t base = current->exp;
	const int64_t difference = std::max<int64_t>(1, info.requiredExp - base);
	if (next != nullptr) {
		const int64_t progress = std::max<int64_t>(0, exp - base);
		const double ratio = static_cast<double>(progress) / static_cast<double>(difference);
		info.percent = std::min(100, static_cast<int>(std::llround(ratio * 100.0)));
	} else {
		info.percent = 100;
	}
	return info;
}

// ---------------------------------------------------------------------------------------------
// kill/death accounting
// ---------------------------------------------------------------------------------------------

bool IgnoreBotDeath(bool countBots, bool attackerBot, bool victimBot) {
	return !countBots && (attackerBot || victimBot);
}

KillDeltas ComputeKillDeltas(const Config& config, bool attackerValid, bool victimValid, bool samePlayer,
							 bool sameTeam, bool sameClan, bool warmup) {
	KillDeltas deltas;
	if (warmup && config.ignoreWarmup) {
		return deltas;
	}
	const bool teamBlocked = config.ignoreSameTeam && sameTeam;
	const bool clanBlocked = config.ignoreSameClan && sameClan;
	if (attackerValid && !samePlayer && !teamBlocked && !clanBlocked) {
		deltas.attacker = true;
		deltas.attackerDelta = config.killReward;
	}
	if (victimValid) {
		deltas.victim = true;
		deltas.victimDelta = -config.deathPenalty;
	}
	return deltas;
}

int32_t ClampBankValue(int64_t current, int64_t delta) {
	int64_t value = current + delta;
	if (value < 0) {
		value = 0;
	}
	if (value > std::numeric_limits<int32_t>::max()) {
		value = std::numeric_limits<int32_t>::max();
	}
	return static_cast<int32_t>(value);
}

bool BankDeltaApplies(int64_t current, int64_t delta) {
	const int64_t clampedCurrent = std::clamp<int64_t>(current, 0, std::numeric_limits<int32_t>::max());
	const int64_t clampedNew =
		std::clamp<int64_t>(current + delta, 0, std::numeric_limits<int32_t>::max());
	return clampedNew != clampedCurrent;
}

std::string_view BankAddSql() {
	return "UPDATE clans_main SET bank = CASE WHEN bank > 2147483647 - ? THEN 2147483647 "
		   "ELSE bank + ? END WHERE id = ?";
}

std::string_view BankSubtractSql() {
	return "UPDATE clans_main SET bank = CASE WHEN bank >= ? THEN bank - ? ELSE 0 END WHERE id = ?";
}

std::string_view KillsIncrementSql() {
	return "UPDATE clans_players SET kills = kills + 1, lastactivity = ? WHERE auth = ?";
}

std::string_view DeathsIncrementSql() {
	return "UPDATE clans_players SET deaths = deaths + 1, lastactivity = ? WHERE auth = ?";
}

std::string_view TouchMemberSql() {
	return "UPDATE clans_players SET name = ?, lastactivity = ? WHERE auth = ?";
}

std::string_view TouchActivitySql() {
	return "UPDATE clans_players SET lastactivity = ? WHERE auth = ?";
}

// ---------------------------------------------------------------------------------------------
// menu-action SQL (mirrors forward/data.php, race-safe single-statement guards)
// ---------------------------------------------------------------------------------------------

std::string JoinClanSql(bool requireOpenType, bool mysql) {
	std::string sql =
		"INSERT INTO clans_players (auth, name, clan_id, joined, lastactivity) "
		"SELECT ?, ?, ?, ?, ?";
	if (mysql) {
		sql += " FROM DUAL";
	}
	sql += " WHERE NOT EXISTS (SELECT 1 FROM clans_players WHERE auth = ?)";
	if (requireOpenType) {
		sql += " AND EXISTS (SELECT 1 FROM clans_main m WHERE m.id = ? AND m.type = 1)";
	}
	sql +=
		" AND (SELECT COUNT(*) FROM clans_players p WHERE p.clan_id = ?) < "
		"(SELECT m.slots + COALESCE(u.slots_up, 0) * ? FROM clans_main m "
		"LEFT JOIN clans_upgrade u ON u.clan_id = m.id WHERE m.id = ?)";
	return sql;
}

std::string ApplyRequestSql(bool mysql) {
	std::string sql =
		"INSERT INTO clans_request (clan_id, steamid, name, date) SELECT ?, ?, ?, ?";
	if (mysql) {
		sql += " FROM DUAL";
	}
	sql +=
		" WHERE EXISTS (SELECT 1 FROM clans_main m WHERE m.id = ? AND m.type = 3)"
		" AND NOT EXISTS (SELECT 1 FROM clans_players WHERE auth = ?)"
		" AND NOT EXISTS (SELECT 1 FROM clans_request WHERE clan_id = ? AND steamid = ?)";
	return sql;
}

std::string_view LeaveClanSql() {
	return "DELETE FROM clans_players WHERE clan_id = ? AND auth = ? "
		   "AND auth <> (SELECT owner FROM clans_main WHERE id = ?)";
}

std::string_view KickMemberSql() {
	return "DELETE FROM clans_players WHERE clan_id = ? AND auth = ? "
		   "AND auth <> (SELECT owner FROM clans_main WHERE id = ?) AND auth <> ?";
}

std::string_view AcceptRequestSql() {
	return "INSERT INTO clans_players (auth, name, clan_id, joined, lastactivity, role_id) "
		   "SELECT r.steamid, r.name, r.clan_id, ?, ?, "
		   "(SELECT id FROM clans_roles WHERE clan_id = r.clan_id AND type = 2 ORDER BY id LIMIT 1) "
		   "FROM clans_request r WHERE r.id = ? AND r.clan_id = ? "
		   "AND NOT EXISTS (SELECT 1 FROM clans_players p WHERE p.auth = r.steamid) "
		   "AND (SELECT COUNT(*) FROM clans_players p2 WHERE p2.clan_id = r.clan_id) < "
		   "(SELECT m.slots + COALESCE(u.slots_up, 0) * ? FROM clans_main m "
		   "LEFT JOIN clans_upgrade u ON u.clan_id = m.id WHERE m.id = r.clan_id)";
}

std::string_view DeleteRequestByIdSql() {
	return "DELETE FROM clans_request WHERE id = ? AND clan_id = ?";
}

std::string_view DeleteAcceptedRequestSql() {
	return "DELETE FROM clans_request WHERE id = ? AND clan_id = ? AND steamid = ? "
		   "AND EXISTS (SELECT 1 FROM clans_players WHERE auth = ? AND clan_id = ?)";
}

std::string_view DeleteRequestByPlayerSql() {
	return "DELETE FROM clans_request WHERE clan_id = ? AND steamid = ?";
}

std::string_view DisbandMainSql() {
	return "DELETE FROM clans_main WHERE id = ? AND owner = ?";
}

std::string_view DisbandLockSql() {
	return "SELECT id FROM clans_main WHERE id = ? FOR UPDATE";
}

std::vector<std::string_view> DisbandCascadeSql(bool mysql, bool deleteLogs) {
	// The site's deleteClanCascade order in one transaction. The MySQL form starts with a row
	// lock: the site may transfer the clan or change anything at any time, and without the lock
	// the guarded deletes could pass on their snapshot while the guarded main delete sees the
	// new owner. Every cascade statement repeats the owner guard because plgdb cannot branch on
	// a statement result - when the lock found no owned clan, all deletes must be no-ops.
	// SQLite (tests/dev) serializes writers, so its first DELETE takes the write lock and the
	// guard stays valid until the commit. The last statement reports whether the clan is gone.
	std::vector<std::string_view> statements;
	if (mysql) {
		statements.push_back(DisbandLockSql());
	}
	statements.insert(statements.end(), {
		"DELETE FROM clans_players WHERE clan_id = ? "
		"AND EXISTS (SELECT 1 FROM clans_main m WHERE m.id = ? AND m.owner = ?)",
		"DELETE FROM clans_roles WHERE clan_id = ? "
		"AND EXISTS (SELECT 1 FROM clans_main m WHERE m.id = ? AND m.owner = ?)",
		"DELETE FROM clans_request WHERE clan_id = ? "
		"AND EXISTS (SELECT 1 FROM clans_main m WHERE m.id = ? AND m.owner = ?)",
		"DELETE FROM clans_reactions WHERE clan_id = ? "
		"AND EXISTS (SELECT 1 FROM clans_main m WHERE m.id = ? AND m.owner = ?)",
		"DELETE FROM clans_views WHERE clan_id = ? "
		"AND EXISTS (SELECT 1 FROM clans_main m WHERE m.id = ? AND m.owner = ?)",
		"DELETE FROM clans_upgrade WHERE clan_id = ? "
		"AND EXISTS (SELECT 1 FROM clans_main m WHERE m.id = ? AND m.owner = ?)",
	});
	if (deleteLogs) {
		statements.emplace_back(
			"DELETE FROM clans_logs WHERE clan_id = ? "
			"AND EXISTS (SELECT 1 FROM clans_main m WHERE m.id = ? AND m.owner = ?)");
	}
	statements.emplace_back(DisbandMainSql());
	statements.emplace_back(DisbandRemainingSql());
	return statements;
}

std::string_view DisbandRemainingSql() {
	return "SELECT COUNT(*) AS remaining FROM clans_main WHERE id = ?";
}

std::string_view InsertLogSql() {
	return "INSERT INTO clans_logs (clan_id, action, value, created, status) VALUES (?, ?, ?, ?, 1)";
}

// ---------------------------------------------------------------------------------------------
// clans_logs JSON (PHP json_encode(..., JSON_UNESCAPED_UNICODE))
// ---------------------------------------------------------------------------------------------

std::string JsonEncodePhp(std::string_view value) {
	std::string out;
	out.reserve(value.size() + 8);
	for (std::size_t index = 0; index < value.size(); ++index) {
		const char character = value[index];
		const unsigned char byte = static_cast<unsigned char>(character);
		// PHP escapes U+2028/U+2029 unless JSON_UNESCAPED_LINE_TERMINATORS is also set; the site
		// passes only JSON_UNESCAPED_UNICODE.
		if (byte == 0xE2 && index + 2 < value.size()
			&& static_cast<unsigned char>(value[index + 1]) == 0x80
			&& (static_cast<unsigned char>(value[index + 2]) == 0xA8
				|| static_cast<unsigned char>(value[index + 2]) == 0xA9)) {
			out += static_cast<unsigned char>(value[index + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
			index += 2;
			continue;
		}
		switch (byte) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '/': out += "\\/"; break; // PHP escapes slashes unless JSON_UNESCAPED_SLASHES
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (byte < 0x20) {
				char buffer[8] = {};
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned int>(byte));
				out += buffer;
			} else {
				out.push_back(character);
			}
			break;
		}
	}
	return out;
}

std::string LogValuePlayer(std::string_view auth) {
	return "{\"player\":\"" + JsonEncodePhp(auth) + "\"}";
}

std::string LogValueKick(std::string_view auth, std::string_view reason) {
	return "{\"player\":\"" + JsonEncodePhp(auth) + "\",\"reason\":\"" + JsonEncodePhp(reason) + "\"}";
}

// ---------------------------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------------------------

std::vector<std::string> TokenizeCommand(std::string_view line) {
	std::vector<std::string> tokens;
	std::string current;
	bool quoted = false;
	for (const char character : line) {
		if (character == '"') {
			quoted = !quoted;
			continue;
		}
		if (!quoted && (character == ' ' || character == '\t' || character == '\r' || character == '\n')) {
			if (!current.empty()) {
				tokens.push_back(std::move(current));
				current.clear();
			}
			continue;
		}
		current.push_back(character);
	}
	if (!current.empty()) {
		tokens.push_back(std::move(current));
	}
	return tokens;
}

Subcommand ResolveSubcommand(std::string_view input) {
	static const std::map<std::string, Subcommand> map = [] {
		std::map<std::string, Subcommand> result;
		const auto add = [&result](Subcommand command, std::initializer_list<const char*> names) {
			for (const char* name : names) {
				result[LowerAscii(name)] = command;
			}
		};
		add(Subcommand::Info, {"info", "status"});
		add(Subcommand::Members, {"members", "list", "m"});
		add(Subcommand::Top, {"top", "tops"});
		add(Subcommand::Chat, {"chat", "gc", "c"});
		add(Subcommand::Help, {"help", "h", "?"});
		return result;
	}();
	const auto found = map.find(LowerAscii(TrimSpaces(input)));
	return found == map.end() ? Subcommand::Unknown : found->second;
}

const char* SubcommandCanonical(Subcommand command) {
	switch (command) {
	case Subcommand::Help: return "help";
	case Subcommand::Info: return "info";
	case Subcommand::Members: return "members";
	case Subcommand::Top: return "top";
	case Subcommand::Chat: return "chat";
	case Subcommand::Unknown: return "unknown";
	}
	return "unknown";
}

const char* SubcommandAliases(Subcommand command) {
	switch (command) {
	case Subcommand::Help: return "help, h, ?";
	case Subcommand::Info: return "info, status";
	case Subcommand::Members: return "members, list, m";
	case Subcommand::Top: return "top, tops";
	case Subcommand::Chat: return "chat, gc, c";
	case Subcommand::Unknown: return "";
	}
	return "";
}

// ---------------------------------------------------------------------------------------------
// s2sdk helpers
// ---------------------------------------------------------------------------------------------

bool IsValidEntityHandle(int32_t handle) {
	return handle != 0 && handle != -1;
}

std::string ResolveMenuTypeName(std::string_view playerChoice, bool hudRegistered,
								bool wasdRegistered) {
	if (!playerChoice.empty()) {
		// hudmenu's answer: only the known names are accepted, and "hud"/"wasd" only while the
		// matching backend is really registered (a stale/foreign answer falls through to the
		// mapped type).
		if (playerChoice == "chat" || playerChoice == "centerhtml" || playerChoice == "button") {
			return std::string(playerChoice);
		}
		if (playerChoice == "hud") {
			return hudRegistered ? std::string("hud") : std::string("button");
		}
		if (playerChoice == "wasd") {
			return wasdRegistered ? std::string("wasd") : std::string("button");
		}
	}
	return hudRegistered ? std::string("hud") : std::string("button");
}

bool IsChatTrigger(char c) {
	return c == '!' || c == '/';
}

std::string StripChatTrigger(std::string_view name) {
	if (!name.empty() && IsChatTrigger(name.front())) {
		name.remove_prefix(1);
	}
	return std::string(name);
}

} // namespace k4
