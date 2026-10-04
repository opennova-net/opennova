// The project, the import and Files as a modder meets them (ADR 0046, the UX round's project lane), over a
// session on a fake process platform: a kind named in words, the folders a file of each kind lands in, the
// files a filter lists (the files query as Files lists them), and a file's card (what it is, where a build
// puts it, what it names and who names it, a wave's sound), with the requests that show it and play it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/project_files.h>
#include <editor/session/file_card.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string repo() { return test_paths_repo_root(__FILE__); }

// A query's answer (null with the error printed when it is refused).
JsonValue ask(ProjectSession &session, const char *name, JsonValue args) {
	std::string error;
	JsonValue out = session.query(name, args, error);
	if (!error.empty()) std::fprintf(stderr, "query %s: %s\n", name, error.c_str());
	return out;
}

JsonValue object_of(std::initializer_list<std::pair<const char *, JsonValue>> members) {
	JsonValue out = JsonValue::make_object();
	for (const auto &[key, value] : members) out.set(key, value);
	return out;
}

// The paths a files answer lists, in order.
std::vector<std::string> listed_paths(const JsonValue &answer) {
	std::vector<std::string> out;
	if (const JsonValue *files = answer.get("files"))
		for (const JsonValue &file : files->array) out.push_back(file.get_string("path", ""));
	return out;
}

// A kind is named in a modder's words: its label or its token, any case, singular or plural, "kind:".
int test_kind_named_by() {
	bool only = true;
	TEST_EXPECT(asset_kind_named_by("texture", &only) == AssetKind::Texture && !only);
	TEST_EXPECT(asset_kind_named_by("Textures") == AssetKind::Texture);
	TEST_EXPECT(asset_kind_named_by("kind:wave", &only) == AssetKind::Wave && only);
	TEST_EXPECT(asset_kind_named_by("sound bank") == AssetKind::SoundBank);
	TEST_EXPECT(asset_kind_named_by("sound_banks") == AssetKind::SoundBank);
	TEST_EXPECT(asset_kind_named_by("AI profiles") == AssetKind::AiProfile);
	TEST_EXPECT(asset_kind_named_by("item definitions") == AssetKind::ItemDefs);
	TEST_EXPECT(asset_kind_named_by("Particle effects") == AssetKind::Particles);
	TEST_EXPECT(asset_kind_named_by("dbuggy") == AssetKind::kCount);
	TEST_EXPECT(asset_kind_named_by("") == AssetKind::kCount);
	// Every kind says what it is to the game.
	for (size_t i = 0; i < kAssetKindCount; ++i) TEST_EXPECT(*asset_kind_row(static_cast<AssetKind>(i)).about);
	return 0;
}

// The folders a made or imported file of a kind lands in: a texture, a wave and a sound bank, a particle
// file and an AI profile have theirs (1,253 of 2,387 files of the audit's import sat at the root); an
// import source sits with the files its name makes; the files the game reads from its own folder by a
// fixed name stay at the root.
int test_folders() {
	const AssetScan none;
	TEST_EXPECT(import_destination(none, "boxtile.tga", AssetKind::Texture) == "textures/boxtile.tga");
	TEST_EXPECT(import_destination(none, "ampv168.wav", AssetKind::Wave) == "sounds/ampv168.wav");
	TEST_EXPECT(import_destination(none, "menu.LWF", AssetKind::SoundBank) == "sounds/menu.LWF");
	TEST_EXPECT(import_destination(none, "smoke.ptl", AssetKind::Particles) == "particles/smoke.ptl");
	TEST_EXPECT(import_destination(none, "D_buggy.aip", AssetKind::AiProfile) == "ai/D_buggy.aip");
	TEST_EXPECT(import_destination(none, "my_logo.png", AssetKind::ImportSource) == "textures/my_logo.png");
	TEST_EXPECT(import_destination(none, "game.cfg", AssetKind::Config) == "game.cfg");
	TEST_EXPECT(import_destination(none, "score.ini", AssetKind::Score) == "score.ini");
	return 0;
}

// A project holding the sound bank fixture (it names tone.wav and two waves the project lacks), its wave, a
// texture whose name holds "texture" and two others: the files a filter lists, and the cards.
struct SoundProject {
	editor_test::TempProjectDir dir{"opennova_editor_project_lane"};
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	bool made = false;
	SoundProject() {
		if (!session.handle(request::new_project(dir.file("project"), "Sounds"))) return;
		session.run_operations();
		const std::string root = session.view().project.root;
		made = editor_test::write_bytes(root + "/sounds/menu.lwf", test_io::read_file(repo() + "/fixtures/lwf/menu.lwf")) &&
		       editor_test::write_bytes(root + "/sounds/tone.wav", test_io::read_file(repo() + "/fixtures/lwf/tone.wav")) &&
		       editor_test::write_text(root + "/sounds/bad.wav", "not a wave") &&
		       editor_test::write_text(root + "/texture_notes.txt", "which texture goes where") &&
		       editor_test::write_text(root + "/rootpic.tga", "not a picture") &&
		       editor_test::write_text(root + "/textures/zz.pcx", "not a picture") &&
		       editor_test::write_text(root + "/textures/aa.tga", "not a picture");
		session.handle(request::rescan());
		session.run_operations();
	}
};

// The files query lists what Files' filter lists: the paths holding the text, then the files of a kind it
// names; "kind:" that kind alone; a kind's token narrows.
int test_files_filter() {
	SoundProject project;
	TEST_EXPECT(project.made);
	ProjectSession &session = project.session;
	const JsonValue named = ask(session, "files", object_of({{"text", JsonValue::make_string("texture")}}));
	const std::vector<std::string> paths = listed_paths(named);
	// The paths holding the word (the textures' folder, a text named after them), then the texture whose
	// path does not hold it, found by its kind; never a wave.
	TEST_EXPECT(paths.size() == 4 && paths.back() == "rootpic.tga");
	TEST_EXPECT(std::find(paths.begin(), paths.end(), "texture_notes.txt") != paths.end() &&
	            std::find(paths.begin(), paths.end(), "textures/aa.tga") != paths.end() &&
	            std::find(paths.begin(), paths.end(), "textures/zz.pcx") != paths.end());
	const std::vector<std::string> waves = listed_paths(ask(session, "files", object_of({{"text", JsonValue::make_string("kind:wave")}})));
	TEST_EXPECT(waves == std::vector<std::string>({"sounds/bad.wav", "sounds/tone.wav"}));
	const std::vector<std::string> banks = listed_paths(ask(session, "files", object_of({{"kind", JsonValue::make_string("sound_bank")}})));
	TEST_EXPECT(banks == std::vector<std::string>({"sounds/menu.lwf"}));
	std::string error;
	session.query("files", object_of({{"kind", JsonValue::make_string("no_such_kind")}}), error);
	TEST_EXPECT(!error.empty());
	return 0;
}

// A file's card: a sound bank names its waves (the one the project has a wave, the others missing) and is
// packed into resource.pff; the wave decodes as the game decodes it, is named by the bank and packs into
// localres.pff; about_file shows the card in Files (a RevealFile event, tag 1); play_sound and stop_sound
// are the Shell's.
int test_file_card() {
	SoundProject project;
	TEST_EXPECT(project.made);
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const FileCard bank = file_card(v, "menu.lwf");
	TEST_EXPECT(bank.found && bank.kind == AssetKind::SoundBank && bank.path == "sounds/menu.lwf");
	TEST_EXPECT(bank.build == "Packed into resource.pff." && !bank.about.empty() && !bank.opens);
	bool tone = false, missing = false;
	for (const FileCard::Named &named : bank.names) {
		if (named.file == "sounds/tone.wav") tone = named.wave && named.status == ReferenceStatus::Present;
		if (named.value.find("MSOVR_2") != std::string::npos) missing = named.status == ReferenceStatus::Missing;
	}
	TEST_EXPECT(tone && missing);
	const FileCard wave = file_card(v, "sounds/tone.wav");
	TEST_EXPECT(wave.found && wave.wave && wave.sound.decoded && wave.sound.rate > 0 && wave.sound.channels >= 1 &&
	            wave.sound.seconds > 0.0);
	TEST_EXPECT(wave.build == "Packed into localres.pff.");
	bool named_by_bank = false;
	for (const FileCard::User &user : wave.named_by) named_by_bank = named_by_bank || user.file == "sounds/menu.lwf";
	TEST_EXPECT(named_by_bank);
	// A wave the game cannot decode says why.
	const FileCard broken = file_card(v, "bad.wav");
	TEST_EXPECT(broken.found && broken.wave && !broken.sound.decoded && !broken.sound.error.empty());
	TEST_EXPECT(!file_card(v, "nothing.wav").found);
	// The wire's card.
	const JsonValue card = ask(session, "file_card", object_of({{"path", JsonValue::make_string("tone.wav")}}));
	TEST_EXPECT(card.get_bool("found", false) && card.get("sound") && card.get("sound")->get_bool("decoded", false));
	TEST_EXPECT(card.get("named_by") && !card.get("named_by")->array.empty());
	// about_file: Files selects the file and opens its card.
	TEST_EXPECT(session.handle(request::about_file("tone.wav")));
	const std::vector<ViewEvent> held(v.events.held().begin(), v.events.held().end());
	TEST_EXPECT(!held.empty() && held.back().kind == ViewEventKind::RevealFile && held.back().path == "sounds/tone.wav" &&
	            held.back().tag == 1);
	// Playing is the Shell's.
	TEST_EXPECT(!session.handle(request::play_sound("tone.wav")) && !session.handle(request::stop_sound()));
	TEST_EXPECT(request_kind_row(EditorRequestKind::PlaySound).served_by == ServedBy::Shell);
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_kind_named_by();
	failed += test_folders();
	failed += test_files_filter();
	failed += test_file_card();
	if (failed == 0) std::printf("editor_project_lane: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
