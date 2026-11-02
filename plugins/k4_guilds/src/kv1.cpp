#include "kv1.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace k4 {
namespace {

char Lower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (Lower(a[i]) != Lower(b[i])) {
			return false;
		}
	}
	return true;
}

std::string_view TrimSpaces(std::string_view text) {
	const auto begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos) {
		return {};
	}
	const auto end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}

// --- the KV1 tokenizer and grammar --------------------------------------------------------------
//
//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
//
// The grammar is the one the original's KeyValues (and pisexcompat's re-implementation) accepted:
// quoted and unquoted tokens, // and /* */ comments anywhere between tokens, \" \\ \n \r \t escapes
// inside quotes (an unknown escape is kept verbatim), and a key that carries both a value and a
// block. A malformed file is rejected as a whole: an unterminated string, an unterminated block
// comment, a missing '}' or a stray top-level '}' set `m_failed` and ParseKv1 returns nullopt -
// never a half-read tree that would silently override the loader's defaults.
class Parser {
public:
	explicit Parser(std::string_view text) : m_text(text) {}

	bool Parse(KvNode& root) {
		ParseNodes(root, /*insideBlock=*/false);
		return !m_failed;
	}

private:
	// Whitespace, // line comments and /* */ block comments between tokens.
	void SkipSpace() {
		while (m_pos < m_text.size() && !m_failed) {
			const char c = m_text[m_pos];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++m_pos;
				continue;
			}
			if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
				m_pos += 2;
				while (m_pos < m_text.size() && m_text[m_pos] != '\n') {
					++m_pos;
				}
				continue;
			}
			if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '*') {
				const std::size_t end = m_text.find("*/", m_pos + 2);
				if (end == std::string_view::npos) {
					m_failed = true; // unterminated block comment
					m_pos = m_text.size();
					return;
				}
				m_pos = end + 2;
				continue;
			}
			break;
		}
	}

	// One token; false at the end of the input or at a '}' (nothing was read then).
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (m_failed || m_pos >= m_text.size() || m_text[m_pos] == '}') {
			return false;
		}
		out.clear();
		if (m_text[m_pos] == '"') {
			++m_pos;
			while (m_pos < m_text.size()) {
				const char c = m_text[m_pos++];
				if (c == '\\' && m_pos < m_text.size()) {
					const char esc = m_text[m_pos++];
					switch (esc) {
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					default:
						// Unknown escapes are kept verbatim, like the original's parser.
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			m_failed = true; // unterminated string
			return true;     // the caller stops on m_failed
		}
		while (m_pos < m_text.size()) {
			const char c = m_text[m_pos];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			// A comment starts a new token boundary: "value/*c*/" reads as "value".
			if (c == '/' && m_pos + 1 < m_text.size() &&
				(m_text[m_pos + 1] == '/' || m_text[m_pos + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++m_pos;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return !m_failed && m_pos < m_text.size() && m_text[m_pos] == c;
	}

	void ParseNodes(KvNode& parent, bool insideBlock) {
		while (!m_failed) {
			SkipSpace();
			if (m_failed) {
				return;
			}
			if (m_pos >= m_text.size()) {
				if (insideBlock) {
					m_failed = true; // missing '}'
				}
				return;
			}
			if (m_text[m_pos] == '}') {
				if (!insideBlock) {
					m_failed = true; // stray top-level '}'
				}
				++m_pos;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return; // end of input or a '}' handled above (ReadToken may have consumed nothing)
			}
			if (m_failed) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: no inclusion, skip the line.
				while (m_pos < m_text.size() && m_text[m_pos] != '\n') {
					++m_pos;
				}
				continue;
			}
			parent.children.push_back(KvNode{});
			// Filled through an index: the recursion below may reallocate parent.children.
			const std::size_t index = parent.children.size() - 1;
			parent.children[index].name = std::move(key);
			if (!PeekIs('{')) {
				if (m_failed) {
					return;
				}
				std::string value;
				if (ReadToken(value)) {
					if (m_failed) {
						return;
					}
					parent.children[index].value = std::move(value);
					parent.children[index].hasValue = true;
				}
			}
			if (PeekIs('{')) {
				if (m_failed) {
					return;
				}
				parent.children[index].isBlock = true;
				++m_pos;
				KvNode child = std::move(parent.children[index]);
				ParseNodes(child, /*insideBlock=*/true);
				parent.children[index] = std::move(child);
				if (m_failed) {
					return;
				}
			}
		}
	}

	std::string_view m_text;
	std::size_t m_pos = 0;
	bool m_failed = false;
};

// A UTF-8 BOM is not part of the first key of the file (the production files are CRLF, but a BOM
// is the one byte order mark a KV1 file may carry).
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

} // namespace

const KvNode* KvNode::Find(std::string_view key) const {
	if (key.empty()) {
		return nullptr;
	}
	for (const KvNode& child : children) {
		if (EqualNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

std::string_view KvNode::GetString(std::string_view key, std::string_view def) const {
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	return found->value;
}

int KvNode::GetInt(std::string_view key, int def) const {
	const std::string_view text = TrimSpaces(GetString(key));
	if (text.empty()) {
		return def;
	}
	int number = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
	if (result.ec != std::errc{}) {
		// Valve falls back to atoi semantics ("5 // comment", "1.5", "abc" -> 0/partial).
		return std::atoi(std::string(text).c_str());
	}
	return number;
}

float KvNode::GetFloat(std::string_view key, float def) const {
	const std::string_view text = TrimSpaces(GetString(key));
	if (text.empty()) {
		return def;
	}
	const std::string copy(text);
	char* end = nullptr;
	const float value = std::strtof(copy.c_str(), &end);
	if (end == copy.c_str()) {
		return def;
	}
	return value;
}

std::optional<KvNode> ParseKv1(std::string_view text) {
	if (text.size() >= kUtf8Bom.size() && text.compare(0, kUtf8Bom.size(), kUtf8Bom) == 0) {
		text.remove_prefix(kUtf8Bom.size());
	}
	KvNode root;
	Parser parser(text);
	if (!parser.Parse(root)) {
		return std::nullopt;
	}
	// A single top-level block is the file's root ("QuakeSounds" { ... }), as in Valve's
	// loader: `LoadKv1File(...)->GetInt("default_volume")` reads the key *inside* the block.
	if (root.children.size() == 1 && root.children.front().isBlock) {
		return std::move(root.children.front());
	}
	return root;
}

std::optional<KvNode> LoadKv1File(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::ostringstream content;
	content << file.rdbuf();
	return ParseKv1(content.str());
}

} // namespace k4
