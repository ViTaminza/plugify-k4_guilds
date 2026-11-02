// k4_guilds package.sh tests — no engine: the real script is run from an isolated copy of the
// plugin directory (the checkout itself, whose assets/ are read-only inputs), with temp
// BUILD_DIR/OUT_DIR and fake build artifacts.
//
// Covered:
//   * the package layout: manifest, library, config.json, the databases.cfg.example template and
//     the phrases file, and never a databases.cfg (deploy merges with cp -n, so an empty-credential
//     file would take the place of the real connection);
//   * no credentials are written by the script (the template's user/pass stay empty);
//   * the OUT_DIR guard: a path outside BUILD_DIR, BUILD_DIR itself, `/`, a symlink that resolves
//     into another tree and a path inside a K4_GUILDS_SERVER_ROOTS tree are all refused before
//     anything is deleted;
//   * the script reads no file outside plugins/k4_guilds/ (it copies exactly the checked-in
//     assets, so a foreign/decoy file cannot leak into the public package).
//
// Run by ./test.sh (build/tests/k4_guilds_package_tests).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "k4_files.hpp"

#ifndef K4_GUILDS_TEST_SOURCE_DIR
#define K4_GUILDS_TEST_SOURCE_DIR "."
#endif

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

std::string ReadText(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) return {};
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

void WriteText(const std::string& path, std::string_view text) {
	const std::filesystem::path parent = std::filesystem::path(path).parent_path();
	std::error_code error;
	if (!parent.empty()) std::filesystem::create_directories(parent, error);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << text;
}

// Runs package.sh with the given environment; returns the exit status and captures stdout+stderr.
int RunPackage(const std::string& build, const std::string& out, const std::string& serverRoots,
			   std::string& output) {
	const std::string script = std::string(K4_GUILDS_TEST_SOURCE_DIR) + "/package.sh";
	const std::string command = "BUILD_DIR='" + build + "' OUT_DIR='" + out + "' K4_GUILDS_SERVER_ROOTS='" +
								serverRoots + "' bash '" + script + "' 2>&1";
	FILE* pipe = ::popen(command.c_str(), "r");
	if (pipe == nullptr) return -1;
	char buffer[4096];
	output.clear();
	while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
	const int status = ::pclose(pipe);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool Exists(const std::string& path) {
	std::error_code error;
	return std::filesystem::exists(path, error);
}

} // namespace

int main() {
	const std::string base = "/tmp/k4_guilds_pkg_" + std::to_string(::getpid());
	std::error_code error;
	std::filesystem::remove_all(base, error);
	const std::string build = base + "/build";
	const std::string out = build + "/package/k4_guilds";
	const std::string pluginBuild = build + "/plugins/k4_guilds";

	WriteText(pluginBuild + "/libk4_guilds.so", "fake shared object\n");
	WriteText(pluginBuild + "/k4_guilds.pplugin", "{\"name\":\"k4_guilds\"}\n");
	// A decoy outside the plugin sources must never be copied: the script only reads its assets.
	WriteText(base + "/decoy-config.json", "{\"secret\":\"must-not-leak\"}\n");

	std::string output;
	Check(RunPackage(build, out, "", output) == 0, "package.sh succeeds with a valid OUT_DIR");
	Check(Exists(out + "/k4_guilds.pplugin"), "manifest copied");
	Check(Exists(out + "/bin/libk4_guilds.so"), "library copied");
	Check(Exists(out + "/configs/k4_guilds/config.json"), "config.json copied");
	Check(Exists(out + "/configs/k4_guilds/databases.cfg.example"), "the template is shipped");
	Check(!Exists(out + "/configs/k4_guilds/databases.cfg"),
		  "no databases.cfg is shipped (deploy merges with cp -n)");
	Check(Exists(out + "/configs/k4_guilds/translations/k4_guilds.phrases.txt"), "phrases copied");
	Check(output.find("decoy") == std::string::npos, "no foreign file name in the output");
	Check(!Exists(out + "/configs/k4_guilds/decoy-config.json"), "a decoy file is not copied");
	{
		const k4::DatabaseConfigResult shipped =
			k4::ParseDatabaseConfig(ReadText(out + "/configs/k4_guilds/databases.cfg.example"), "k4_guilds");
		Check(shipped.found, "the shipped template has the k4_guilds section");
		Check(shipped.settings.user.empty() && shipped.settings.password.empty(),
			  "the shipped template carries no credentials");
	}
	Check(ReadText(out + "/configs/k4_guilds/config.json").find("secret") == std::string::npos,
		  "the shipped config carries no secret");

	// Guard: OUT_DIR outside BUILD_DIR.
	Check(RunPackage(build, base + "/outside", "", output) != 0, "OUT_DIR outside BUILD_DIR is refused");
	Check(Exists(pluginBuild + "/libk4_guilds.so"), "the refused run deleted nothing");
	// Guard: OUT_DIR == BUILD_DIR (would `rm -rf` the whole build tree).
	Check(RunPackage(build, build, "", output) != 0, "OUT_DIR == BUILD_DIR is refused");
	Check(Exists(pluginBuild + "/libk4_guilds.so"), "the build tree survived");
	// Guard: filesystem root.
	Check(RunPackage(build, "/", "", output) != 0, "OUT_DIR == / is refused");
	// Guard: a server tree listed in K4_GUILDS_SERVER_ROOTS.
	Check(RunPackage(build, build + "/server/package", build + "/server", output) != 0,
		  "an OUT_DIR inside a server tree is refused");
	// Guard: a symlink inside the build tree pointing outside it is resolved first.
	std::filesystem::create_directories(base + "/escape", error);
	std::filesystem::create_directory_symlink(base + "/escape", build + "/link", error);
	Check(RunPackage(build, build + "/link/package", "", output) != 0,
		  "a symlink escaping BUILD_DIR is refused");
	Check(!Exists(base + "/escape/package"), "the symlinked target was not written");

	std::filesystem::remove_all(base, error);
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
