#include "k4_files.hpp"

#include "kv1.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace k4 {
namespace {

constexpr std::string_view kFallbackLanguage = "en";

bool StartsWith(std::string_view text, std::string_view prefix) {
	return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string_view StripPrefix(std::string_view text, std::string_view prefix) {
	return StartsWith(text, prefix) ? text.substr(prefix.size()) : std::string_view {};
}

std::vector<std::string> Candidates(std::string_view path, std::string_view configsDir,
									std::string_view dataDir, std::string_view pluginName,
									bool translationsOnly) {
	std::vector<std::string> out;
	if (path.empty()) {
		return out;
	}
	const std::string raw(path);
	const bool absolute = raw.front() == '/';
	out.push_back(raw); // as given (absolute, or relative to the server working directory)
	if (absolute) {
		return out;
	}

	const std::string configDir = PluginConfigDir(configsDir, pluginName);
	const std::string translations = PluginTranslationsDir(configsDir, pluginName);
	const std::string data = PluginDataDir(dataDir, pluginName);

	// "addons/configs/<plugin>/x.cfg" / "addons/translations/x.phrases.txt" / "addons/data/x" are
	// the Metamod spellings; strip the fixed prefix and treat the rest as a relative path.
	std::string_view relative = path;
	bool fromTranslations = translationsOnly;
	bool fromData = false;
	if (auto rest = StripPrefix(path, "addons/configs/"); !rest.empty()) {
		relative = rest;
		// "admin_system/core.ini" -> "core.ini" when it is our own config dir.
		if (!pluginName.empty() && StartsWith(rest, std::string(pluginName) + "/")) {
			relative = rest.substr(pluginName.size() + 1);
		}
	} else if (auto rest = StripPrefix(path, "addons/translations/"); !rest.empty()) {
		relative = rest;
		fromTranslations = true;
	} else if (auto rest = StripPrefix(path, "addons/data/"); !rest.empty()) {
		relative = rest;
		fromData = true;
	} else if (auto rest = StripPrefix(path, "addons/"); !rest.empty()) {
		relative = rest;
	}

	const std::string rel(relative);
	if (fromTranslations) {
		out.push_back(JoinPath(translations, rel));
	} else if (fromData) {
		out.push_back(JoinPath(data, rel));
	} else {
		out.push_back(JoinPath(configDir, rel));
		out.push_back(JoinPath(translations, rel));
		out.push_back(JoinPath(data, rel));
	}
	return out;
}

std::string FirstExisting(const std::vector<std::string>& candidates) {
	for (const std::string& candidate : candidates) {
		if (!candidate.empty() && FileExists(candidate)) {
			return candidate;
		}
	}
	return {};
}

// --- cs2-menus' colour table (pisexcompat's copy, same order, same bytes) ----------------------

struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
												  {"{WHITE}", "\x01"},
												  {"{RED}", "\x02"},
												  {"{LIGHTPURPLE}", "\x03"},
												  {"{GREEN}", "\x04"},
												  {"{LIME}", "\x05"},
												  {"{LIGHTGREEN}", "\x06"},
												  {"{DARKRED}", "\x07"},
												  {"{GRAY}", "\x08"},
												  {"{LIGHTOLIVE}", "\x09"},
												  {"{OLIVE}", "\x10"},
												  {"{LIGHTBLUE}", "\x0B"},
												  {"{BLUE}", "\x0C"},
												  {"{PURPLE}", "\x0E"},
												  {"{LIGHTRED}", "\x0F"},
												  {"{GRAYBLUE}", "\x0A"},
												  {"\\n", "\xe2\x80\xa9"}}};

std::string LowerAscii(std::string_view text) {
	std::string lower;
	lower.reserve(text.size());
	for (const char c : text) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	return lower;
}

} // namespace

// --- paths ------------------------------------------------------------------------------------

std::string JoinPath(std::string_view base, std::string_view name) {
	if (base.empty()) {
		return std::string(name);
	}
	if (name.empty()) {
		return std::string(base);
	}
	std::string out(base);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (name.front() == '/') {
		name.remove_prefix(1);
	}
	out.append(name);
	return out;
}

bool FileExists(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

std::optional<std::string> ReadTextFile(const std::string& path) {
	if (path.empty()) {
		return std::nullopt;
	}
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

std::string PluginConfigDir(std::string_view configsDir, std::string_view pluginName) {
	return pluginName.empty() ? std::string(configsDir) : JoinPath(configsDir, pluginName);
}

std::string PluginTranslationsDir(std::string_view configsDir, std::string_view pluginName) {
	return JoinPath(PluginConfigDir(configsDir, pluginName), "translations");
}

std::string PluginDataDir(std::string_view dataDir, std::string_view pluginName) {
	return pluginName.empty() ? std::string(dataDir) : JoinPath(dataDir, pluginName);
}

std::string ResolveConfigPath(std::string_view path, std::string_view configsDir,
							  std::string_view dataDir, std::string_view pluginName) {
	if (path.empty()) {
		return {};
	}
	const std::vector<std::string> candidates =
		Candidates(path, configsDir, dataDir, pluginName, /*translationsOnly=*/false);
	if (std::string resolved = FirstExisting(candidates); !resolved.empty()) {
		return resolved;
	}
	// Nothing on disk: hand back the most likely location so callers can log a useful path.
	return candidates.size() > 1 ? candidates[1] : candidates.front();
}

std::string ResolveTranslationsPath(std::string_view file, std::string_view configsDir,
									std::string_view dataDir, std::string_view pluginName) {
	(void)dataDir; // pisexcompat's translation resolver only uses the config tree
	if (file.empty()) {
		return {};
	}
	std::string name(file);
	// Pisex's LoadTranslations() takes "menus.phrases" and appends ".txt".
	if (name.size() < 4 || name.compare(name.size() - 4, 4, ".txt") != 0) {
		name += ".txt";
	}
	const std::string translations = PluginTranslationsDir(configsDir, pluginName);
	const std::string configDir = PluginConfigDir(configsDir, pluginName);
	const std::string direct =
		FirstExisting({name, JoinPath(translations, name), JoinPath(configDir, name)});
	if (!direct.empty()) {
		return direct;
	}
	// "addons/translations/k4_guilds.phrases.txt" and friends.
	std::string_view relative = name;
	if (auto rest = StripPrefix(name, "addons/translations/"); !rest.empty()) {
		relative = rest;
	}
	return JoinPath(translations, relative);
}

std::string ResolveDataPath(std::string_view path, std::string_view dataDir,
							std::string_view pluginName) {
	if (path.empty()) {
		return {};
	}
	std::string file(path);
	if (auto rest = StripPrefix(file, "addons/data/"); !rest.empty()) {
		file = std::string(rest);
	}
	return JoinPath(PluginDataDir(dataDir, pluginName), file);
}

// --- phrases ----------------------------------------------------------------------------------

std::string NormalizeLanguage(std::string_view language) {
	const std::string lower = LowerAscii(language);
	// Source language names -> the codes used by the Pisex phrase files.
	if (lower == "russian" || lower == "ru") return "ru";
	if (lower == "english" || lower == "en") return "en";
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") return "ua";
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") return "pt-BR";
	if (lower == "spanish" || lower == "es") return "es";
	if (lower == "german" || lower == "de") return "de";
	if (lower == "french" || lower == "fr") return "fr";
	if (lower == "polish" || lower == "pl") return "pl";
	if (lower == "turkish" || lower == "tr") return "tr";
	if (lower == "schinese" || lower == "zh-cn") return "zh-CN";
	return std::string(language);
}

std::string DetectLanguage(const std::optional<std::string>& coreConfigText,
						   std::string_view sdkLanguage) {
	if (coreConfigText.has_value()) {
		if (const std::optional<KvNode> core = ParseKv1(*coreConfigText); core.has_value()) {
			const std::string value(core->GetString("ServerLang", ""));
			if (!value.empty()) {
				return NormalizeLanguage(value);
			}
		}
	}
	if (!sdkLanguage.empty()) {
		return NormalizeLanguage(sdkLanguage);
	}
	return std::string(kFallbackLanguage);
}

bool LoadPhrases(std::string_view text, std::string_view language, PhraseMap& out) {
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root.has_value()) {
		return false;
	}
	const std::string selected =
		language.empty() ? std::string(kFallbackLanguage) : std::string(language);
	const std::string fallback(kFallbackLanguage);

	for (const KvNode& key : root->children) {
		if (!key.isBlock) {
			continue; // the original iterated GetFirstTrueSubKey() (blocks only)
		}
		std::string value;
		if (const KvNode* localized = key.Find(selected); localized != nullptr && localized->hasValue) {
			value = localized->value;
		}
		if (value.empty() && selected != fallback) {
			if (const KvNode* english = key.Find(fallback); english != nullptr && english->hasValue) {
				value = english->value;
			}
		}
		if (value.empty()) {
			// Last resort: any language the file has, so a partially translated file still shows
			// something instead of an empty chat line.
			for (const KvNode& candidate : key.children) {
				if (candidate.hasValue) {
					value = candidate.value;
					break;
				}
			}
		}
		out[key.name] = std::move(value);
	}
	return true;
}

bool LoadPhrasesFile(const std::string& path, std::string_view language, PhraseMap& out) {
	const std::optional<std::string> text = ReadTextFile(path);
	if (!text.has_value()) {
		return false;
	}
	return LoadPhrases(*text, language, out);
}

std::string GetPhrase(const PhraseMap& phrases, std::string_view key) {
	const auto found = phrases.find(std::string(key));
	return found == phrases.end() ? std::string() : found->second;
}

std::string StripColorTags(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		const std::string_view replacement =
			color.tag == "\\n" ? std::string_view("\n") : std::string_view();
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), replacement);
			pos += replacement.size();
		}
	}
	return out;
}

std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

// --- databases.cfg ----------------------------------------------------------------------------

DatabaseConfigResult ParseDatabaseConfig(std::string_view text, std::string_view section) {
	DatabaseConfigResult result;
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root.has_value()) {
		result.error = "databases.cfg is not valid KeyValues";
		return result;
	}
	if (section.empty()) {
		result.error = "the database section name is empty";
		return result;
	}
	const KvNode* entry = root->Find(section);
	if (entry == nullptr || !entry->isBlock) {
		return result; // found == false: no such connection in the file
	}
	result.found = true;
	result.settings.driver = LowerAscii(entry->GetString("driver", "mysql"));
	result.settings.host = std::string(entry->GetString("host", "127.0.0.1"));
	result.settings.port = entry->GetInt("port", 3306);
	result.settings.user = std::string(entry->GetString("user", ""));
	result.settings.password = std::string(entry->GetString("pass", ""));
	result.settings.database = std::string(entry->GetString("database", ""));
	result.settings.sqlitePath = std::string(entry->GetString("sqlite_path", ""));
	return result;
}

} // namespace k4
