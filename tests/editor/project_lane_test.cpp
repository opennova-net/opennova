// The project, the import and Files as a modder meets them (ADR 0046, the UX round's project lane), over a
// session on a fake process platform: a kind named in words, the folders a file of each kind lands in, the
// files a filter lists (the files query as Files lists them), a file's card (what it is, where a build puts
// it, what it names and who names it, a wave's sound), with the requests that show it and play it; a game
// install checked, New project's install, the recent projects as the welcome page lists them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <filesystem>

#include <base/io/os_path.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/install_check.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_plan_groups.h>
#include <editor/import/import_run.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <formats/pff/pff.h>
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
#include "editor/import_test_support.h"
#include "editor/png_test_support.h"
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
// localres.pff; about_file shows the card in Files, the workspace's; play_sound and stop_sound are the
// session's, the Shell reporting how the sound goes; the card closed stops its sound.
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
	// about_file: Files selects the file, and its card opens, the workspace's (the MCP gaps lane).
	TEST_EXPECT(session.handle(request::about_file("tone.wav")));
	const std::vector<ViewEvent> held(v.events.held().begin(), v.events.held().end());
	TEST_EXPECT(!held.empty() && held.back().kind == ViewEventKind::RevealFile && held.back().path == "sounds/tone.wav");
	TEST_EXPECT(v.workspace.card.path == "sounds/tone.wav" && v.documents.file_selected.path == "sounds/tone.wav");
	// The sound is the session's (the MCP gaps lane): play_sound starts a play the Shell takes by its serial,
	// what the Shell reports seen; a name no wave has, refused.
	TEST_EXPECT(request_kind_row(EditorRequestKind::PlaySound).served_by == ServedBy::Session &&
	            request_kind_row(EditorRequestKind::StopSound).served_by == ServedBy::Session);
	TEST_EXPECT(session.handle(request::play_sound("tone.wav")) && session.outcome().done());
	const uint64_t serial = v.workspace.sound.serial;
	TEST_EXPECT(v.workspace.sound.path == "sounds/tone.wav" && v.workspace.sound.state == WorkspaceView::SoundState::Starting &&
	            serial != 0);
	session.report_sound(serial, WorkspaceView::SoundState::Playing);
	TEST_EXPECT(v.workspace.sound.state == WorkspaceView::SoundState::Playing);
	session.report_sound(serial + 7, WorkspaceView::SoundState::Failed, "a play no one asked");
	TEST_EXPECT(v.workspace.sound.state == WorkspaceView::SoundState::Playing && v.workspace.sound.error.empty());
	TEST_EXPECT(session.handle(request::stop_sound()) && v.workspace.sound.state == WorkspaceView::SoundState::Stopped);
	session.report_sound(serial, WorkspaceView::SoundState::Ended);
	TEST_EXPECT(v.workspace.sound.state == WorkspaceView::SoundState::Stopped); // a stopped play's report passed over
	TEST_EXPECT(session.handle(request::play_sound("menu.lwf")) && !session.outcome().done() &&
	            v.workspace.sound.serial == serial && v.workspace.sound.path == "sounds/tone.wav");
	// The card closed (set_workspace): the sound it played stops.
	TEST_EXPECT(session.handle(request::play_sound("tone.wav")) && v.workspace.sound.serial == serial + 1);
	TEST_EXPECT(session.handle(request::set_workspace(R"({"card": {"path": ""}})")) && session.outcome().done());
	TEST_EXPECT(v.workspace.card.path.empty() && v.workspace.sound.state == WorkspaceView::SoundState::Stopped);
	// The wire's: the card and the sound in the workspace section.
	session.handle(request::about_file("tone.wav"));
	session.handle(request::play_sound("tone.wav"));
	const JsonValue section = ask(session, "state", object_of({{"sections", [] {
		JsonValue list = JsonValue::make_array();
		list.push(JsonValue::make_string("workspace"));
		return list;
	}()}}));
	const JsonValue *workspace = section.get("workspace");
	TEST_EXPECT(workspace && workspace->get("card") && workspace->get("card")->get_string("path", "") == "sounds/tone.wav" &&
	            workspace->get("sound") && workspace->get("sound")->get_string("state", "") == "starting" &&
	            workspace->get("sound")->get_string("path", "") == "sounds/tone.wav");
	// The review's L5: a sound read before is taken as it is while its file stands, read again once it moved,
	// and a wave past the card's cap is said to be too large, nothing read.
	FileCard::Sound known = wave.sound;
	known.rate = 12345;
	TEST_EXPECT(file_card(v, "sounds/tone.wav", &known).sound.rate == 12345);
	known.size += 1;
	TEST_EXPECT(file_card(v, "sounds/tone.wav", &known).sound.rate == wave.sound.rate);
	const std::string root = v.project.root;
	{
		std::ofstream large(opennova::io::os_path(root + "/sounds/huge.wav"), std::ios::binary);
		large.seekp(std::streamoff(kWaveCardBytes) + 16);
		large.put('\0');
	}
	// L3: the card says what the build says of a file it refuses or leaves out.
	TEST_EXPECT(editor_test::write_text(root + "/saves/player.sav", "a save") &&
	            editor_test::write_text(root + "/extra/mine.pff", "an archive"));
	session.handle(request::rescan());
	session.run_operations();
	const FileCard huge = file_card(v, "huge.wav");
	TEST_EXPECT(huge.found && huge.wave && !huge.sound.decoded && huge.sound.error.find("MB at most") != std::string::npos);
	// The demo round's bug 9: while the graph has not read the project's files as the scan lists them (a
	// file landed: a Create, an import) the card says its names are being read (its lists the graph's as
	// far as it has read), on the wire too; once read, it does not; nor during an edit's validation, which
	// the graph keeps up with (the review's Y7: no flicker on each keystroke).
	TEST_EXPECT(!wave.reading && !file_card(v, "sounds/tone.wav").reading);
	session.handle(request::create_file("Extra.mnu", asset_kind_token(AssetKind::Menu)));
	TEST_EXPECT(v.activity.validation.files_unread && file_card(v, "sounds/tone.wav").reading &&
	            ask(session, "file_card", object_of({{"path", JsonValue::make_string("tone.wav")}})).get_bool("reading", false));
	session.run_operations();
	TEST_EXPECT(!v.activity.validation.files_unread && !file_card(v, "sounds/tone.wav").reading);
	const Document *menu = session.document_for("Extra.mnu");
	TEST_EXPECT(menu != nullptr);
	if (menu) {
		Edit rename;
		rename.address = menu->address_at("0/window:0");
		rename.field = "name";
		rename.value = std::string("RENAMED");
		session.handle(request::edit_record(menu->path(), rename)); // the validation it leaves due shows running
		TEST_EXPECT(v.activity.validation.running && !file_card(v, "sounds/tone.wav").reading &&
		            !ask(session, "file_card", object_of({{"path", JsonValue::make_string("tone.wav")}})).get_bool("reading", false));
		session.run_operations();
		TEST_EXPECT(!v.activity.validation.running && !file_card(v, "sounds/tone.wav").reading);
	}
	const FileCard save = file_card(v, "player.sav");
	TEST_EXPECT(save.found && save.build.find("left out") != std::string::npos && save.build.find("beside the archives") == std::string::npos);
	const FileCard archive = file_card(v, "mine.pff");
	TEST_EXPECT(archive.found && archive.build.rfind("A build refuses it: ", 0) == 0);
	return 0;
}

// An archive at `path` holding the 4-byte files `names`.
bool write_archive(const std::string &path, const std::vector<std::string> &names) {
	static const uint8_t bytes[] = {1, 2, 3, 4};
	std::error_code ec;
	std::filesystem::create_directories(opennova::io::os_path(path).parent_path(), ec);
	std::vector<opennova::pff::PffWriteEntry> entries;
	for (const std::string &name : names) entries.push_back({name.c_str(), bytes, sizeof(bytes), 0, 0, 0});
	return opennova::pff::pff_write_archive(path.c_str(), opennova::pff::PFF_FORMAT_PFF3, entries.data(), entries.size()) ==
	       opennova::pff::PFF_WRITE_OK;
}

// A folder made to look like a game install: the boot table's three archives, resource.pff holding `files`
// files and the other two one each (so `files` + 2 in all), the game's program beside them when `executable`.
bool fake_install(const std::string &dir, size_t files, bool executable) {
	std::vector<std::string> names;
	for (size_t i = 0; i < files; ++i) names.push_back("file" + std::to_string(i) + ".tga");
	if (!editor_test::write_text(dir + "/readme.txt", "an install")) return false;
	if (!write_archive(dir + "/resource.pff", names) || !write_archive(dir + "/language.pff", {"lang.tga"}) ||
	    !write_archive(dir + "/localres.pff", {"local.tga"}))
		return false;
	return !executable || editor_test::write_text(dir + "/Jointops.exe", "MZ");
}

// A folder read as a game install: one whose archives mount says what it serves and whether the game's
// program is there; one holding none is no game; a folder that is not there says so; none named says the
// project can still take files from the disk.
int test_install_check() {
	editor_test::TempProjectDir dir("opennova_editor_install_check");
	const std::string install = dir.file("Joint Operations");
	TEST_EXPECT(fake_install(install, 3, true));
	const InstallCheck found = check_install(install, "jo");
	TEST_EXPECT(found.exists && found.ok() && found.files == 5 && found.executable && found.expansions.empty());
	TEST_EXPECT(found.root == absolute_install_path(install));
	TEST_EXPECT(found.words().find(": 5 files.") != std::string::npos && found.words().find("Jointops.exe") == std::string::npos);
	const std::string bare = dir.file("Bare");
	TEST_EXPECT(fake_install(bare, 1, false));
	TEST_EXPECT(check_install(bare, "jo").ok() && check_install(bare, "jo").words().find("No Jointops.exe beside them") != std::string::npos);
	const std::string empty = dir.file("Empty");
	TEST_EXPECT(editor_test::write_text(empty + "/notes.txt", "nothing of the game"));
	const InstallCheck none = check_install(empty, "jo");
	TEST_EXPECT(none.exists && !none.ok() && none.words().rfind("No game here", 0) == 0);
	// The review's L11: a language pack alone is no install (the game boots on any one archive, the editor
	// imports from the whole game); archives that do not open say so; a build the editor made is a project's.
	const std::string pack = dir.file("Pack");
	TEST_EXPECT(write_archive(pack + "/language.pff", {"words.tga"}));
	const InstallCheck language = check_install(pack, "jo");
	TEST_EXPECT(!language.ok() && language.mounts &&
	            language.missing_archives == std::vector<std::string>({"localres.pff", "resource.pff"}) &&
	            language.words() == "Not the whole game: no localres.pff or resource.pff here (a language pack, or an install in part).");
	const std::string broken = dir.file("Broken");
	TEST_EXPECT(editor_test::write_text(broken + "/resource.pff", "not an archive") &&
	            editor_test::write_text(broken + "/language.pff", "not one either"));
	const InstallCheck unopened = check_install(broken, "jo");
	TEST_EXPECT(!unopened.ok() && unopened.archives_present &&
	            unopened.words().rfind("The game's archives here do not open: language.pff and resource.pff are there", 0) == 0);
	const std::string built = dir.file("Built");
	TEST_EXPECT(fake_install(built, 1, false) && editor_test::write_text(built + "/build.json", "{}"));
	TEST_EXPECT(!check_install(built, "jo").ok() && check_install(built, "jo").build &&
	            check_install(built, "jo").words().rfind("This is a build the editor made", 0) == 0);
	const std::string cached = dir.file("Mod/.opennova/build/play/abc");
	TEST_EXPECT(fake_install(cached, 1, false));
	TEST_EXPECT(!check_install(cached, "jo").ok() && check_install(cached, "jo").build);
	TEST_EXPECT(!check_install(dir.file("Nowhere"), "jo").exists &&
	            check_install(dir.file("Nowhere"), "jo").words().rfind("There is no folder", 0) == 0);
	TEST_EXPECT(!check_install("", "jo").ok() && check_install("", "jo").words().rfind("No game install chosen", 0) == 0);
	return 0;
}

// New project with a game install: the folder checked, then the editor's install and the project's (its
// local.json); a folder that holds no game refused, nothing made; check_install on the wire; the recent
// projects with their titles and games, one whose folder no longer holds a project not found.
int test_new_project_install() {
	editor_test::TempProjectDir dir("opennova_editor_new_project_install");
	const std::string install = dir.file("JO");
	TEST_EXPECT(fake_install(install, 2, true));
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	{
		ProjectSession session(platform, preferences);
		const SessionView &v = session.view();
		// A folder that holds no game: refused, nothing made, the editor's install as it was.
		const std::string empty = dir.file("NotAGame");
		TEST_EXPECT(editor_test::write_text(empty + "/x.txt", "x"));
		EditorRequest wrong = request::new_project(dir.file("Refused"), "Refused");
		wrong.game_install = empty;
		session.handle(wrong);
		session.run_operations();
		TEST_EXPECT(!v.project.open && !std::filesystem::exists(opennova::io::os_path(dir.file("Refused"))));
		TEST_EXPECT(v.project.install_check.root == absolute_install_path(empty) && !v.project.install_check.ok());
		TEST_EXPECT(preferences.preferences().game_install.empty());
		// The review's M3: why, where the welcome page shows it.
		TEST_EXPECT(v.project.refused.rfind("The project could not be created: No game here", 0) == 0);
		// The review's L2: a project refused after its install was checked leaves the editor's install as it was.
		EditorRequest expansion = request::new_expansion_project(dir.file("Exp"), "Exp", "myexp", "jox99");
		expansion.game_install = install;
		session.handle(expansion);
		session.run_operations();
		TEST_EXPECT(!v.project.open && v.project.install_check.ok() && !v.project.refused.empty() &&
		            preferences.preferences().game_install.empty());
		// The install: the project opens on it, and the editor keeps it.
		EditorRequest made = request::new_project(dir.file("Mod"), "My Mod");
		made.game_install = install;
		session.handle(made);
		session.run_operations();
		TEST_EXPECT(v.project.open && v.project.retail_directory == absolute_install_path(install) && v.project.refused.empty());
		TEST_EXPECT(preferences.preferences().game_install == absolute_install_path(install) &&
		            v.project.editor_install == absolute_install_path(install));
		LocalSettings local;
		Diagnostic finding;
		TEST_EXPECT(load_local_settings(ProjectPaths::for_root(v.project.root), local, finding) &&
		            local.game_install == absolute_install_path(install));
		// check_install: the folder named, else the editor's.
		session.handle(request::check_install(dir.file("Nowhere")));
		TEST_EXPECT(!v.project.install_check.exists);
		session.handle(request::check_install());
		TEST_EXPECT(v.project.install_check.ok() && v.project.install_check.files == 4);
		// A second project, then the first's folder emptied: the recent projects as the welcome page lists them.
		EditorRequest other = request::new_project(dir.file("Other"), "Other Mod");
		session.handle(other);
		session.run_operations();
		TEST_EXPECT(v.project.recent_details.size() == 2 && v.project.recent_details[0].title == "Other Mod" &&
		            v.project.recent_details[1].title == "My Mod" && v.project.recent_details[1].found &&
		            !v.project.recent_details[1].game.empty());
		session.handle(request::close_project());
	}
	std::error_code ec;
	std::filesystem::remove(opennova::io::os_path(dir.file("Mod") + "/" + kProjectFileName), ec);
	ProjectSession again(platform, preferences);
	const SessionView &v = again.view();
	TEST_EXPECT(v.project.recent_details.size() == 2 && v.project.recent_details[0].found && !v.project.recent_details[1].found);
	// The review's L10: nothing mounted as the session starts; the welcome page's field asks for its check.
	TEST_EXPECT(v.project.install_check.root.empty() && v.project.editor_install == absolute_install_path(install));
	// The review's M3: an Open of a recent project that no longer reads drops it, saying so.
	again.handle(request::open_project(dir.file("Mod")));
	TEST_EXPECT(!v.project.open && v.project.recent_projects.size() == 1 &&
	            v.project.refused.find("off the recent projects") != std::string::npos);
	return 0;
}

// The review's M1: an Open with a session's own install (open_project's game_install) plays and imports
// from it while it is open and never writes it: closing (with the documents moved, which writes the
// workspace) and quitting leave local.json naming the project's own; Project settings' Apply of the
// project's own install drops the session's. The review's L13: a project whose folder went while it was open
// gets no `.opennova/` made at its old path as it closes. L15e: a session that keeps no workspace (the
// command line's) reopens none and writes none.
int test_session_install_and_workspace() {
	editor_test::TempProjectDir dir("opennova_editor_session_install");
	const std::string own = dir.file("JO");
	const std::string other = dir.file("Other JO");
	TEST_EXPECT(fake_install(own, 1, true) && fake_install(other, 1, true));
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	EditorRequest made = request::new_project(dir.file("Mod"), "Mod");
	made.game_install = own;
	session.handle(made);
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::string root = v.project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	session.handle(request::close_project());
	EditorRequest opened = request::open_project(root);
	opened.game_install = other;
	session.handle(opened);
	session.run_operations();
	TEST_EXPECT(v.project.open && v.project.retail_directory == absolute_install_path(other));
	const AssetEntry *strings = v.project.scan->find("gametext.bin");
	TEST_EXPECT(strings != nullptr);
	if (strings) session.handle(request::open_document(strings->relative_path));
	session.handle(request::close_project());
	LocalSettings local;
	Diagnostic finding;
	TEST_EXPECT(load_local_settings(paths, local, finding) && local.game_install == absolute_install_path(own) &&
	            local.open_documents.size() == 1);
	session.handle(opened);
	session.run_operations();
	session.handle(request::quit());
	TEST_EXPECT(load_local_settings(paths, local, finding) && local.game_install == absolute_install_path(own));
	// Apply of the project's own install in the settings: the session's dropped, nothing else written.
	ProjectSettingsChange change;
	change.game_install = own;
	session.handle(request::apply_project_settings(change));
	TEST_EXPECT(v.project.retail_directory == absolute_install_path(own));
	// L15e: no workspace kept: the document local.json lists stays closed, and a close writes nothing.
	session.handle(request::close_project());
	session.set_workspace_kept(false);
	session.handle(request::open_project(root));
	session.run_operations();
	TEST_EXPECT(v.project.open && v.documents.open.empty());
	session.handle(request::close_project());
	TEST_EXPECT(load_local_settings(paths, local, finding) && local.open_documents.size() == 1);
	session.set_workspace_kept(true);
	// L13: the folder gone while the project is open: nothing made at its old path as it closes.
	session.handle(request::open_project(root));
	session.run_operations();
	TEST_EXPECT(v.project.open && !v.documents.open.empty());
	if (strings) session.handle(request::close_document(v.documents.open.front()->path()));
	std::error_code ec;
	std::filesystem::remove_all(opennova::io::os_path(root), ec);
	TEST_EXPECT(!ec);
	session.handle(request::close_project());
	TEST_EXPECT(!std::filesystem::exists(opennova::io::os_path(root)));
	return 0;
}

// A project reopens as it was left: the documents open when it closed, in their order, the one active, and
// the record selected in each (kept in .opennova/local.json as the project closes and as the editor quits);
// a file gone since is passed over, the others reopened, Output saying how many.
int test_reopen_as_left() {
	editor_test::TempProjectDir dir("opennova_editor_reopen_as_left");
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	TEST_EXPECT(session.handle(request::new_project(dir.file("Mod"), "Mod")));
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::string root = v.project.root;
	const AssetEntry *menu_entry = v.project.scan->find("main.mnu");
	const AssetEntry *strings_entry = v.project.scan->find("gametext.bin");
	TEST_EXPECT(menu_entry && strings_entry);
	const std::string menu_path = menu_entry->relative_path, strings_path = strings_entry->relative_path;
	session.handle(request::open_document(strings_path));
	session.handle(request::open_document(menu_path));
	const Document *menu = session.document_for(menu_path);
	TEST_EXPECT(menu && !menu->rows().empty());
	const Node &screen = *menu->rows().back();
	session.handle(request::select_record(menu_path, {screen.id, screen.kind, 0}));
	const std::string locator = menu->locator({screen.id, screen.kind, 0});
	session.handle(request::open_document(strings_path)); // the table active as the project closes
	TEST_EXPECT(session.handle(request::close_project()));
	LocalSettings local;
	Diagnostic finding;
	TEST_EXPECT(load_local_settings(ProjectPaths::for_root(root), local, finding));
	TEST_EXPECT(local.open_documents.size() == 2 && local.open_documents[0].path == strings_path &&
	            local.open_documents[1].path == menu_path && local.open_documents[1].locator == locator &&
	            local.active_document == strings_path);
	session.handle(request::open_project(root));
	session.run_operations();
	TEST_EXPECT(v.documents.open.size() == 2 && v.documents.open[0]->path() == strings_path &&
	            v.documents.open[1]->path() == menu_path && v.documents.active == strings_path);
	session.handle(request::open_document(menu_path));
	TEST_EXPECT(v.documents.selection.primary.row == session.document_for(menu_path)->address_at(locator).row);
	// Quit keeps them too; a file gone since is passed over.
	session.handle(request::quit());
	TEST_EXPECT(load_local_settings(ProjectPaths::for_root(root), local, finding) && local.active_document == menu_path);
	session.handle(request::close_project());
	std::error_code ec;
	std::filesystem::remove(opennova::io::os_path(root + "/" + strings_path), ec);
	session.handle(request::open_project(root));
	session.run_operations();
	TEST_EXPECT(v.documents.open.size() == 1 && v.documents.active == menu_path);
	bool said = false;
	for (size_t i = 0; i < v.activity.output.size(); ++i) said = said || v.activity.output[i].find("Reopened 1 file") != std::string::npos;
	TEST_EXPECT(said);
	return 0;
}

// The import's plan as a modder reads it: what wanted a file in the words of the wanting file's type, a
// chosen file the project has said to be the same or not, the rows by what they come for (the chosen menu
// with the kinds of the files it brings under it), and a plan of many chosen files and none found by kind.
int test_import_plan_words() {
	using import_test::font;
	using import_test::image;
	using import_test::row_named;
	using import_test::screen;
	using import_test::window;
	import_test::Project project("opennova_editor_lane_plan");
	const std::string root = project.root();
	const std::string art = project.dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", screen("A", window("BUTTON", "GO", font("arial99") + image("logo.tga")))));
	TEST_EXPECT(editor_test::write_text(art + "/arial99.fnt", "fnt") && editor_test::write_text(art + "/LOGO.TGA", "tga"));
	TEST_EXPECT(editor_test::write_text(root + "/fonts/same.fnt", "same") && editor_test::write_text(art + "/same.fnt", "same") &&
	            editor_test::write_text(root + "/fonts/edited.fnt", "mine!!") &&
	            editor_test::write_text(art + "/edited.fnt", "theirs"));
	project.session.handle(request::rescan());
	project.session.run_operations();
	const ImportPlan plan = project.plan({{art + "/a.mnu", {}}, {art + "/same.fnt", {}}, {art + "/edited.fnt", {}}});
	const ImportPlanRow *arial = row_named(plan, "arial99.fnt");
	const ImportPlanRow *logo = row_named(plan, "LOGO.TGA");
	TEST_EXPECT(arial && logo);
	if (!arial || !logo) return 1;
	// The window by its kind and its names, the field by its title, never a record path or a field's id.
	const std::string words = import_need_text(arial->needed_by);
	std::printf("needed by: %s; %s\n", words.c_str(), import_need_text(logo->needed_by).c_str());
	TEST_EXPECT(words == "a.mnu, window A > GO: Font");
	// A record whose own name says its kind is not said twice.
	TEST_EXPECT(import_need_text(logo->needed_by) == "a.mnu, A > GO > Appearance 1: Image or colour");
	const ImportPlanRow *same = row_named(plan, "same.fnt");
	const ImportPlanRow *edited = row_named(plan, "edited.fnt");
	TEST_EXPECT(same && same->held && same->held_as == ImportPlanRow::Held::Same);
	TEST_EXPECT(edited && edited->held && edited->held_as == ImportPlanRow::Held::Differs);
	// The rows by what they come for: the menu with its font and its texture under it, each chosen font apart.
	const std::vector<ImportPlanGroup> groups = import_plan_groups(plan);
	TEST_EXPECT(groups.size() == 5 && groups[0].chosen() && plan.rows[groups[0].root].name == "a.mnu" &&
	            groups[0].children.size() == 2 && groups[0].files == 3);
	for (const size_t child : groups.empty() ? std::vector<size_t>() : groups[0].children)
		TEST_EXPECT(groups[child].depth == 1 && groups[child].parent == 0 && groups[child].rows.size() == 1 &&
		            (groups[child].kind == AssetKind::Font || groups[child].kind == AssetKind::Texture));
	// Many files chosen and none found: by kind, the largest first.
	std::vector<ImportChoice> many;
	for (int i = 0; i < 22; ++i) {
		const std::string name = art + "/t" + std::to_string(10 + i) + ".tga";
		TEST_EXPECT(editor_test::write_text(name, "texture bytes"));
		many.push_back({name, {}});
	}
	many.push_back({art + "/arial99.fnt", {}});
	const std::vector<ImportPlanGroup> kinds = import_plan_groups(project.plan(many, false));
	TEST_EXPECT(kinds.size() == 2 && kinds[0].kind == AssetKind::Texture && kinds[0].files == 22 &&
	            kinds[0].root == ImportPlanGroup::kNone && kinds[1].kind == AssetKind::Font);
	// The review's L7: a font two chosen menus name: wanted by both, planned under the first, listed under the
	// second too (its `also`), counted once.
	TEST_EXPECT(editor_test::write_text(art + "/b.mnu", screen("B", window("BUTTON", "GO", font("arial99")))));
	const ImportPlan both = project.plan({{art + "/a.mnu", {}}, {art + "/b.mnu", {}}});
	const ImportPlanRow *shared = row_named(both, "arial99.fnt");
	TEST_EXPECT(shared && shared->wanted_by == std::vector<std::string>({"a.mnu", "b.mnu"}));
	const size_t shared_row = shared ? size_t(shared - both.rows.data()) : SIZE_MAX;
	const std::vector<ImportPlanGroup> shared_groups = import_plan_groups(both);
	bool under_a = false, under_b = false;
	for (const ImportPlanGroup &group : shared_groups) {
		if (group.chosen() || group.parent == ImportPlanGroup::kNone) continue;
		const std::string &above = both.rows[shared_groups[group.parent].root].name;
		under_a = under_a || (above == "a.mnu" && std::find(group.rows.begin(), group.rows.end(), shared_row) != group.rows.end());
		under_b = under_b || (above == "b.mnu" && std::find(group.also.begin(), group.also.end(), shared_row) != group.also.end());
	}
	TEST_EXPECT(under_a && under_b && shared_groups[0].files == 3);
	return 0;
}

// The review's L6 and the 64-file cap: a chosen file the project has from an archive compared by its size
// first, then its bytes; the first kHeldCompared held files compared, those past them unknown.
int test_held_compare() {
	using import_test::row_named;
	import_test::Project project("opennova_editor_lane_held");
	const std::string root = project.root();
	const std::string archive = project.dir.file("mod/held.pff");
	TEST_EXPECT(write_archive(archive, {"same.tga", "other.tga"}));
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/same.tga", {1, 2, 3, 4}) &&
	            editor_test::write_bytes(root + "/textures/other.tga", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
	const std::string art = project.dir.file("art");
	std::vector<ImportChoice> chosen{{archive, "same.tga"}, {archive, "other.tga"}};
	for (size_t i = 0; i < kHeldCompared + 2; ++i) {
		const std::string name = "h" + std::to_string(100 + i) + ".tga";
		TEST_EXPECT(editor_test::write_text(art + "/" + name, "held") && editor_test::write_text(root + "/textures/" + name, "held"));
		chosen.push_back({art + "/" + name, {}});
	}
	project.session.handle(request::rescan());
	project.session.run_operations();
	const ImportPlan plan = project.plan(chosen, false);
	const ImportPlanRow *same = row_named(plan, "same.tga");
	const ImportPlanRow *other = row_named(plan, "other.tga");
	TEST_EXPECT(same && same->held && same->held_as == ImportPlanRow::Held::Same);
	TEST_EXPECT(other && other->held && other->held_as == ImportPlanRow::Held::Differs);
	size_t compared = 0, unknown = 0;
	for (const ImportPlanRow &row : plan.rows) {
		if (!row.held) continue;
		if (row.held_as == ImportPlanRow::Held::Unknown) ++unknown;
		else ++compared;
	}
	TEST_EXPECT(compared == kHeldCompared && unknown == 4);
	return 0;
}

// The chooser's facts: a game install's files and an archive's members each with its kind by its name and its
// size as stored; the import_preview query's choices carry them, and its rows their words and groups.
int test_import_choices() {
	editor_test::TempProjectDir dir("opennova_editor_lane_choices");
	const std::string install = dir.file("JO");
	TEST_EXPECT(fake_install(install, 3, true));
	ProjectDocument document;
	document.target_game = "jo";
	std::vector<Diagnostic> findings;
	std::vector<ImportChoiceFacts> facts;
	const std::vector<ImportChoice> choices = list_retail_import_choices(install, document, findings, &facts);
	TEST_EXPECT(choices.size() == 5 && facts.size() == 5 && findings.empty());
	for (const ImportChoiceFacts &fact : facts) TEST_EXPECT(fact.kind == AssetKind::Texture && fact.size == 4);
	facts.clear();
	const std::vector<ImportChoice> members = list_import_choices({install + "/resource.pff"}, findings, &facts);
	TEST_EXPECT(members.size() == 3 && facts.size() == 3 && facts[0].kind == AssetKind::Texture && facts[0].size == 4);
	// Through the session: the game data listed to choose from, each with its kind and size.
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	EditorRequest made = request::new_project(dir.file("Mod"), "Mod");
	made.game_install = install;
	session.handle(made);
	session.run_operations();
	session.handle(request::preview_install_import());
	session.run_operations();
	const JsonValue preview = ask(session, "import_preview", JsonValue::make_object());
	const JsonValue *listed = preview.get("choices");
	TEST_EXPECT(listed && listed->array.size() == 5 && listed->array[0].get_string("kind", "") == "texture" &&
	            listed->array[0].get_number("size", 0) == 4);
	TEST_EXPECT(preview.get("groups") && preview.get("groups")->is_array());
	return 0;
}

// What an import reported stays with the source while it is current (the UX round's project lane): a
// project's PNG whose alpha a PCX drops says so on the pass that imports it and on every pass after (a
// reopened project), and no more once its record keeps the alpha.
int test_import_findings_kept() {
	editor_test::TempProjectDir dir("opennova_editor_lane_findings");
	const std::string root = dir.file("project");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Kept", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	editor_test::PngSpec spec;
	spec.width = spec.height = 2;
	for (uint32_t y = 0; y < 2; ++y) {
		spec.rows.push_back(0);
		for (uint32_t x = 0; x < 2; ++x)
			for (const uint32_t channel : {x * 90, y * 120, 33u, 40u + x * 100}) spec.rows.push_back(uint8_t(channel));
	}
	TEST_EXPECT(editor_test::write_bytes(root + "/art/glow.png", editor_test::make_png(spec)));
	const Importer *importer = importer_for("glow.png");
	TEST_EXPECT(importer != nullptr);
	if (!importer) return 1;
	ImportSidecar record;
	record.importer = importer->id;
	record.version = importer->version;
	record.options["format"] = "pcx";
	TEST_EXPECT(save_import_sidecar(root + "/art/glow.png.import", record, error));
	const auto dropped = [](const ImportRunResult &run) {
		size_t count = 0;
		for (const Diagnostic &d : run.diagnostics) count += d.code() == "import.alpha_dropped" && d.asset == "art/glow.png" ? 1 : 0;
		return count;
	};
	ImportRunResult run = run_imports(paths, doc);
	TEST_EXPECT(run.reimported == 1 && dropped(run) == 1);
	run = run_imports(paths, doc);
	TEST_EXPECT(run.reimported == 0 && dropped(run) == 1);
	TEST_EXPECT(load_import_sidecar(root + "/art/glow.png.import", record, error));
	record.options["format"] = "tga";
	TEST_EXPECT(save_import_sidecar(root + "/art/glow.png.import", record, error));
	run = run_imports(paths, doc);
	TEST_EXPECT(run.reimported == 1 && dropped(run) == 0);
	run = run_imports(paths, doc);
	TEST_EXPECT(run.reimported == 0 && dropped(run) == 0);
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_kind_named_by();
	failed += test_folders();
	failed += test_files_filter();
	failed += test_file_card();
	failed += test_install_check();
	failed += test_new_project_install();
	failed += test_session_install_and_workspace();
	failed += test_reopen_as_left();
	failed += test_import_plan_words();
	failed += test_held_compare();
	failed += test_import_choices();
	failed += test_import_findings_kept();
	if (failed == 0) std::printf("editor_project_lane: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
