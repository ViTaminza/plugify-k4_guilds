// k4_guilds — the file half of the plugin: where its production files are read from and how the
// KeyValues and phrase tables are decoded.
//
// Self-contained (docs/PLUGIN_STANDARD.md §7: no pisexcompat and no helper library), engine-free
// so tests/k4_guilds_tests.cpp covers every branch without a server. The plugin's original files
// are the Metamod installation's, unchanged:
//
//     <configs>/k4_guilds/config.json                       (JSONC, read by k4_core ParseConfig)
//     <configs>/k4_guilds/databases.cfg                     (KeyValues, section k4_guilds)
//     <configs>/k4_guilds/translations/k4_guilds.phrases.txt (KeyValues, "Phrases" tree)
//     <configs>/core.cfg                                    (KeyValues, ServerLang)
//
// The resolution order is pisexcompat's, because that is what the previous build did
// (PLUGIN_STANDARD §7: production files keep their location):
//
//   * ResolveConfigPath()      — the raw spelling first (cwd-relative or absolute, the Metamod
//     tree), then <configs>/<plugin>/<name>, then <configs>/<plugin>/translations/<name>, then
//     <data>/<plugin>/<name>. The first file that exists is authoritative; a malformed file keeps
//     the caller's defaults and never falls through to a decoy (the original's failed
//     LoadFromFile behaved the same way).
//   * ResolveTranslationsPath() — "<file as given>", <translations>/<file>, <config>/<file>
//     (plus the "addons/translations/..." spelling of the same).
//   * ResolveDataPath()        — <data>/<plugin>/<path> with an optional "addons/data/" prefix
//     stripped.
//
// Language: pisexcompat's DetectLanguage() chain — <configs>/core.cfg "ServerLang" first, then
// s2sdk's ServerLanguage, then "en" — with the same Source-name -> phrase-file-code mapping.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace k4 {

// --- paths ------------------------------------------------------------------------------------

std::string JoinPath(std::string_view base, std::string_view name);
bool FileExists(const std::string& path);
std::optional<std::string> ReadTextFile(const std::string& path);

// <configs>/<plugin>; <configs>/<plugin>/translations; <data>/<plugin>.
std::string PluginConfigDir(std::string_view configsDir, std::string_view pluginName);
std::string PluginTranslationsDir(std::string_view configsDir, std::string_view pluginName);
std::string PluginDataDir(std::string_view dataDir, std::string_view pluginName);

// The three pisexcompat resolvers (see the header comment). `configsDir`/`dataDir` are the plugify
// roots (plg::GetConfigsDir()/GetDataDir()); empty ones degrade to the raw spelling.
std::string ResolveConfigPath(std::string_view path, std::string_view configsDir,
							  std::string_view dataDir, std::string_view pluginName);
std::string ResolveTranslationsPath(std::string_view file, std::string_view configsDir,
									std::string_view dataDir, std::string_view pluginName);
std::string ResolveDataPath(std::string_view path, std::string_view dataDir,
							std::string_view pluginName);

// --- phrases ----------------------------------------------------------------------------------

using PhraseMap = std::map<std::string, std::string>;

// Source language name -> the code the Pisex phrase files use ("russian" -> "ru", "english" ->
// "en", ...); an unknown name is returned lower-cased only when it is a known alias, otherwise
// unchanged, exactly like pisexcompat's NormalizeLanguage().
std::string NormalizeLanguage(std::string_view language);

// pisexcompat DetectLanguage(): core.cfg "ServerLang" -> s2sdk ServerLanguage -> "en".
// `coreConfigText` is the file content (std::nullopt when the file does not exist); the sdk
// language is passed in so this stays engine-free.
std::string DetectLanguage(const std::optional<std::string>& coreConfigText,
						   std::string_view sdkLanguage);

// Parses a "Phrases" tree and fills key -> text: the selected language first, then "en", then the
// first language the key has (a partially translated file still shows something instead of a
// blank chat line). A malformed file returns false and leaves `out` untouched.
bool LoadPhrases(std::string_view text, std::string_view language, PhraseMap& out);
bool LoadPhrasesFile(const std::string& path, std::string_view language, PhraseMap& out);

// Empty when the key is missing, like pisex::GetPhrase().
std::string GetPhrase(const PhraseMap& phrases, std::string_view key);

// cs2-menus' colour table, as pisexcompat carried it: StripColorTags() removes the tags (and turns
// the literal "\n" into a real newline) for menu/console text, Colorize() turns them into the
// engine colour bytes for chat.
std::string StripColorTags(std::string_view text);
std::string Colorize(std::string_view text);

// --- databases.cfg ----------------------------------------------------------------------------

struct DatabaseSettings {
	std::string driver = "mysql";
	std::string host = "127.0.0.1";
	int port = 3306;
	std::string user;
	std::string password;
	std::string database;
	std::string sqlitePath;
};

struct DatabaseConfigResult {
	bool found = false;
	std::string error;
	DatabaseSettings settings;
};

// Parses "Databases" { "<section>" { ... } } (the single-top-block file layout). `found` is false
// when the file has no such section; `error` is set for a malformed file. An unsupported driver is
// reported by the caller (plgdb supports mysql/mariadb and sqlite only).
DatabaseConfigResult ParseDatabaseConfig(std::string_view text, std::string_view section);

} // namespace k4
