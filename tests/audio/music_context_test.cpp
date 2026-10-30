// The music context (runtime/audio/music_context.h) over a fake mounted game and a recording
// player: the base and expansion pairs (the expansion's names unconditional, its bank's on-disk
// spelling kept, a missing half silent, never the base pair), the open's order (the teardown
// first, the bank's file before any script read, no script read without a bank file, the explicit
// override by its loose path, a file of no script dropped), the one context at a time, and the
// mission start's seed (Var1..Var12, Var7 = 100) after the game context's open on either outcome.
#include <runtime/audio/music_context.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace opennova;
using audio::MusicContext;
using audio::MusicContextKind;
using audio::MusicPair;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// A mounted game: its expansion, its loose files (path -> on-disk path, matched without case
// under a directory) and its resource-system files (name -> bytes).
struct Files final : audio::MusicFiles {
	std::string mounted_expansion;
	std::string dir = "/game";
	std::map<std::string, std::string> loose; // lower-cased "dir/name" -> on-disk path
	std::map<std::string, std::vector<uint8_t>> resources; // lower-cased name -> bytes
	mutable int reads = 0;

	static std::string lower(std::string s) {
		for (char &c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
		return s;
	}
	std::string expansion() const override { return mounted_expansion; }
	std::string root_dir() const override { return dir; }
	std::string find_loose(const std::string &in, const std::string &name) const override {
		const auto it = loose.find(lower(in + "/" + name));
		return it != loose.end() ? it->second : std::string();
	}
	std::string resolve_file(const std::string &name) const override { return find_loose(dir, name); }
	bool has_file(const std::string &name) const override { return resources.count(lower(name)) != 0; }
	bool read_file(const std::string &name, std::vector<uint8_t> &out) const override {
		++reads;
		const auto it = resources.find(lower(name));
		if (it == resources.end()) return false;
		out = it->second;
		return true;
	}
};

// The director: what each call held, in order.
struct Player final : audio::MusicContextPlayer {
	std::vector<std::string> calls;
	std::map<int, int32_t> vars;
	bool bank = false, script = false;
	int script_count = 1;       // the scripts a byte load finds
	bool file_loads = true;     // whether a loose script file loads
	bool vm_opens_without = false;

	void stop_context() override {
		calls.push_back("stop");
		bank = script = false;
		vars.clear(); // the teardown frees the globals block
	}
	bool load_bank(const std::string &path) override {
		calls.push_back("bank " + path);
		bank = path.find("missing") == std::string::npos;
		return bank;
	}
	bool load_script_file(const std::string &path) override {
		calls.push_back("script_file " + path);
		script = file_loads;
		return file_loads;
	}
	int load_script_bytes(const std::vector<uint8_t> &bytes, const std::string &name) override {
		calls.push_back("script_bytes " + name + " " + std::to_string(bytes.size()));
		script = script_count > 0;
		return script_count;
	}
	void drop_script() override {
		calls.push_back("drop_script");
		script = false;
	}
	bool open_vm() override {
		calls.push_back("open");
		return (bank && script) || vm_opens_without;
	}
	void set_var(int index, int32_t value) override { vars[index] = value; }
};

Files base_game() {
	Files files;
	files.loose["/game/menumus.sbf"] = "/game/MENUMUS.SBF";
	files.loose["/game/gamemus.sbf"] = "/game/GAMEMUS.SBF";
	files.resources["menumus.bin"] = {1, 2, 3};
	files.resources["gamemus.bin"] = {4, 5};
	return files;
}

void test_base_pair() {
	Files files = base_game();
	const MusicPair menu = audio::resolve_music_pair(MusicContextKind::Menu, files);
	CHECK(menu.bank == "/game/MENUMUS.SBF");
	CHECK(menu.script_name == "menumus.bin");
	const MusicPair game = audio::resolve_music_pair(MusicContextKind::Game, files);
	CHECK(game.bank == "/game/GAMEMUS.SBF");
	CHECK(game.script_name == "gamemus.bin");
	// No script in the resource system: the script half is unresolved.
	files.resources.erase("gamemus.bin");
	CHECK(audio::resolve_music_pair(MusicContextKind::Game, files).script_name.empty());
	CHECK(audio::resolve_music_pair(MusicContextKind::None, files).bank.empty());
}

// With an expansion mounted both names are its own, whatever ships: never the base pair.
void test_expansion_pair_is_unconditional() {
	Files files = base_game();
	files.mounted_expansion = "jox01";
	// The bank's on-disk spelling, matched without case.
	files.loose["/game/expansion/jox01/mjox01.sbf"] = "/game/expansion/jox01/MJOX01.SBF";
	const MusicPair menu = audio::resolve_music_pair(MusicContextKind::Menu, files);
	CHECK(menu.bank == "/game/expansion/jox01/MJOX01.SBF");
	CHECK(menu.script_name == "Mjox01.bin");
	// No G<n>.sbf: the canonical expansion path, so the open fails silent.
	const MusicPair game = audio::resolve_music_pair(MusicContextKind::Game, files);
	CHECK(game.bank == "/game/expansion/jox01/Gjox01.sbf");
	CHECK(game.script_name == "Gjox01.bin");
	// The root's directory ending in a separator joins without a second one.
	files.dir = "/game/";
	CHECK(audio::resolve_music_pair(MusicContextKind::Game, files).bank ==
			"/game/expansion/jox01/Gjox01.sbf");
}

// The open: the teardown, the bank's file, then the script's bytes, then the VM.
void test_open_order() {
	Files files = base_game();
	Player player;
	MusicContext context;
	CHECK(context.open(MusicContextKind::Menu, &files, player));
	CHECK(context.kind() == MusicContextKind::Menu);
	CHECK(std::string(audio::music_context_name(context.kind())) == "menu");
	const std::vector<std::string> expected = {
			"stop", "bank /game/MENUMUS.SBF", "script_bytes menumus.bin 3", "open"};
	CHECK(player.calls == expected);
	// The menu context seeds nothing (the shown screen's MUSICVAR selects its section).
	CHECK(player.vars.empty());
}

// No bank file: no script read, the open fails, and no context is kept.
void test_missing_bank_gates_the_script() {
	Files files = base_game();
	files.loose["/game/menumus.sbf"] = "/game/missing/MENUMUS.SBF";
	Player player;
	MusicContext context;
	CHECK(context.open(MusicContextKind::Game, &files, player)); // a context runs first
	player.calls.clear();
	files.reads = 0;
	CHECK(!context.open(MusicContextKind::Menu, &files, player));
	CHECK(files.reads == 0);
	CHECK(context.kind() == MusicContextKind::None);
	const std::vector<std::string> expected = {"stop", "bank /game/missing/MENUMUS.SBF", "open"};
	CHECK(player.calls == expected);
	// An unresolved bank is no file either: no bank load at all.
	files.loose.erase("/game/menumus.sbf");
	player.calls.clear();
	CHECK(!context.open(MusicContextKind::Menu, &files, player));
	CHECK(files.reads == 0);
	CHECK((player.calls == std::vector<std::string>{"stop", "open"}));
}

// A file of no script loads none; an explicit override reads its loose path, never the pair's.
void test_script_rules() {
	Files files = base_game();
	Player player;
	player.script_count = 0;
	MusicContext context;
	CHECK(!context.open(MusicContextKind::Menu, &files, player));
	CHECK(player.calls.back() == "open");
	CHECK(player.calls[player.calls.size() - 2] == "drop_script");

	files.loose["/game/dev.bin"] = "/game/DEV.BIN";
	player = Player();
	files.reads = 0;
	CHECK(context.open(MusicContextKind::Menu, &files, player, "dev.bin"));
	CHECK(files.reads == 0);
	CHECK(player.calls[2] == "script_file /game/DEV.BIN");
	// An override that does not load leaves the open with no script.
	player = Player();
	player.file_loads = false;
	CHECK(!context.open(MusicContextKind::Menu, &files, player, "dev.bin"));
	CHECK(player.calls[3] == "drop_script");
	// An override the root does not resolve reads nothing.
	player = Player();
	CHECK(!context.open(MusicContextKind::Menu, &files, player, "absent.bin"));
	CHECK((player.calls == std::vector<std::string>{"stop", "bank /game/MENUMUS.SBF", "open"}));
	// A bank override names the root's loose file in place of the pair's.
	files.loose["/game/other.sbf"] = "/game/OTHER.SBF";
	player = Player();
	CHECK(context.open(MusicContextKind::Menu, &files, player, "", "other.sbf"));
	CHECK(player.calls[1] == "bank /game/OTHER.SBF");
	// No mounted game: nothing resolves, the open fails.
	player = Player();
	CHECK(!context.open(MusicContextKind::Menu, nullptr, player));
	CHECK((player.calls == std::vector<std::string>{"stop", "open"}));
}

// The game context's seed: Var1..Var12, Var7 = 100, after the open on either outcome.
void test_game_seed() {
	const std::vector<audio::MusicVarSeed> seed = audio::game_music_seed();
	CHECK(seed.size() == 12);
	for (size_t i = 0; i < seed.size(); ++i) {
		CHECK(seed[i].index == static_cast<int>(i) + 1);
		CHECK(seed[i].value == (seed[i].index == 7 ? 100 : 0));
	}
	Files files = base_game();
	Player player;
	MusicContext context;
	CHECK(context.open(MusicContextKind::Game, &files, player));
	CHECK(player.vars.size() == 12);
	CHECK(player.vars[1] == 0);
	CHECK(player.vars[7] == 100);
	// A failed open still writes them (the VM drops them with no context).
	files.loose.erase("/game/gamemus.sbf");
	player = Player();
	CHECK(!context.open(MusicContextKind::Game, &files, player));
	CHECK(player.vars.size() == 12);
	context.stop(player);
	CHECK(context.kind() == MusicContextKind::None);
	CHECK(player.calls.back() == "stop");
}

} // namespace

int main() {
	test_base_pair();
	test_expansion_pair_is_unconditional();
	test_open_order();
	test_missing_bank_gates_the_script();
	test_script_rules();
	test_game_seed();
	if (failures != 0) {
		std::printf("music_context: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("music_context: all cases passed\n");
	return 0;
}
