#pragma once
// Local KeyValues 1 reader of the k4_guilds plugin.
//
// The port is self-contained (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no helper library), so
// the production KeyValues files the plugin reads unchanged - databases.cfg, the
// k4_guilds.phrases.txt translation table and <configs>/core.cfg - are parsed here. The grammar is
// the subset the original's engine KeyValues parser accepted:
//
//   * quoted ("key" "value") and unquoted (key value) tokens,
//   * nested "key" { ... } blocks; a key may carry a value and a block,
//   * // line comments and /* */ block comments, including a trailing comment after a value,
//   * the escapes \" \\ \n \r \t inside quoted strings (an unknown escape stays verbatim),
//   * an optional UTF-8 BOM at the start of the file,
//   * #base / #include / #define lines are skipped instead of followed (no file inclusion).
//
// A malformed file - an unterminated string, an unterminated block comment, a missing '}' or a
// stray top-level '}' - is rejected as a whole (std::nullopt), never returned half-read: the
// loaders must not let a truncated tree silently override their defaults.
//
// Like Valve's loader, a file that is a single "Root" { ... } block is returned as that block, so
// `LoadKv1File(path)->GetString("ServerLang")` reads the key inside "Core" { ... }. Key lookup is
// case-insensitive (Valve's FindKey); the stored spelling is kept.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace k4 {

// One node of the parsed tree: a key with an optional scalar value and optional children.
struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	// True when the node was written as a block ("key" { ... }), including an empty one. The
	// original's FOR_EACH_SUBKEY / GetFirstTrueSubKey iterated blocks only, so the loaders use
	// this flag to tell a block from a scalar key.
	bool isBlock = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive, like Valve's FindKey), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or has no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int, with Valve's atoi fallback ("5x" -> 5, "abc" -> 0); `def` when
	// the child is missing or its value is empty (KeyValues::GetInt(key, def)).
	int GetInt(std::string_view key, int def = 0) const;
	// The child's value as a float, strtof semantics ("1.5x" -> 1.5); `def` when the child is
	// missing, its value is empty or it does not start with a number (KeyValues::GetFloat).
	float GetFloat(std::string_view key, float def = 0.0f) const;
};

// The file text; std::nullopt for malformed input (see the grammar above).
std::optional<KvNode> ParseKv1(std::string_view text);
// ParseKv1 over the bytes of `path`; std::nullopt when the file cannot be read or is malformed.
std::optional<KvNode> LoadKv1File(const std::string& path);

} // namespace k4
